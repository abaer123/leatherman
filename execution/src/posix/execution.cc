#include <leatherman/execution/execution.hpp>
#include <leatherman/file_util/directory.hpp>
#include <leatherman/util/scope_exit.hpp>
#include <leatherman/util/posix/scoped_descriptor.hpp>
#include <leatherman/logging/logging.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <array>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <fcntl.h>
#include <signal.h>

using namespace std;
using namespace leatherman::util;
using namespace leatherman::util::posix;
using namespace leatherman::execution;
using namespace leatherman::logging;
using namespace leatherman::file_util;
using namespace boost::filesystem;

// Declare environ for OSX
extern char** environ;

namespace leatherman { namespace execution {

    void log_execution(string const& file, vector<string> const* arguments);

    const char *const command_shell = "sh";
    const char *const command_args = "-c";

    static uint64_t get_max_descriptor_limit()
    {
        // WARNING: this function is called under vfork
        // See comment below in exec_child in case you're not afraid
#ifdef _SC_OPEN_MAX
        {
            auto open_max = sysconf(_SC_OPEN_MAX);
            if (open_max > 0) {
                return open_max;
            }
        }
#endif  // _SC_OPEN_MAX

#ifdef RLIMIT_NOFILE
        {
            rlimit lim;
            if (getrlimit(RLIMIT_NOFILE, &lim) == 0) {
                return lim.rlim_cur;
            }
        }
#endif  // RLIMIT_NOFILE

#ifdef OPEN_MAX
        return OPEN_MAX;
#else
        return 256;
#endif  // OPEN_MAX
    }

    static volatile bool command_timedout = false;

    static void timer_handler(int signal)
    {
        command_timedout = true;
    }

    static string format_error(string const& message = string(), int error = errno)
    {
        if (message.empty()) {
            return (boost::format("%1% (%2%)") % strerror(error) % error).str();
        }
        return (boost::format("%1%: %2% (%3%).") % message % strerror(error) % error).str();
    }

    static vector<gid_t> get_groups()
    {
        // Query for the number of groups
        auto num = getgroups(0, nullptr);
        if (num < 1) {
            return {};
        }

        // Allocate a buffer for the groups
        vector<gid_t> groups(static_cast<size_t>(num));
        num = getgroups(groups.size(), groups.data());
        if (static_cast<size_t>(num) != groups.size()) {
            return {};
        }
        return groups;
    }

    static bool is_group_member(gid_t gid)
    {
        // Check for primary group
        if (getgid() == gid || getegid() == gid) {
            return true;
        }

        // Get the groups and search for the given gid
        static auto groups = get_groups();
        return find(groups.begin(), groups.end(), gid) != groups.end();
    }

    static bool is_executable(char const* path)
    {
        struct stat fs;
        if (stat(path, &fs) != 0) {
            return false;
        }

        auto euid = geteuid();

        // If effectively running as root, any exec bit will do
        if (euid == 0) {
            return fs.st_mode & (S_IXUSR | S_IXGRP | S_IXOTH);
        }

        // If the file is effectively owned, check for user exec bit
        if (fs.st_uid == euid) {
            return fs.st_mode & S_IXUSR;
        }

        // If the file is owned by a group we're a member of, check for the group exec bit
        if (is_group_member(fs.st_gid)) {
            return fs.st_mode & S_IXGRP;
        }

        // Lastly check for "others" exec bit
        return fs.st_mode & S_IXOTH;
    }

    string which(string const& file, vector<string> const& directories)
    {
        // If the file is already absolute, return it if it's executable
        path p = file;
        boost::system::error_code ec;
        if (p.is_absolute()) {
            return is_regular_file(p, ec) && is_executable(p.c_str()) ? p.string() : string();
        }

        // Otherwise, check for an executable file under the given search paths
        for (auto const& dir : directories) {
            path p = path(dir) / file;
            if (is_regular_file(p, ec) && is_executable(p.c_str())) {
                return p.string();
            }
        }
        return {};
    }

    // Represents information about a pipe
    struct pipe
    {
        pipe(string pipe_name, int desc, function<bool(string const&)> const& cb) :
            name(std::move(pipe_name)),
            descriptor(desc),
            callback(cb)
        {
        }

        const string name;
        int descriptor;
        string buffer;
        function<bool(string const&)> const& callback;
    };

    static void rw_from_child(pid_t child, array<pipe, 2>& pipes, pair<scoped_descriptor, string> input, uint32_t timeout)
    {
        // Each pipe is a tuple of descriptor, buffer to use to read data, and a callback to call when data is read
        // The input pair is a descriptor and text to write to it
        fd_set read_set, write_set;
        while (!command_timedout) {
            FD_ZERO(&read_set);
            FD_ZERO(&write_set);

            // Set up the descriptors and buffers to select upon
            int max = -1;
            for (auto& pipe : pipes) {
                if (pipe.descriptor == -1) {
                    continue;
                }
                FD_SET(pipe.descriptor, &read_set);
                if (pipe.descriptor > max) {
                    max = pipe.descriptor;
                }
                pipe.buffer.resize(4096);
            }
            if (input.first != -1) {
                FD_SET(input.first, &write_set);
                if (input.first > max) {
                    max = input.first;
                }
            }
            if (max == -1) {
                // All pipes closed; we're done
                return;
            }

            // If using a timeout, timeout after 500ms to check whether or not the command itself timed out
            timeval read_timeout = {};
            read_timeout.tv_usec = 500000;
            int result = select(max + 1, &read_set, &write_set, nullptr, timeout ? &read_timeout : nullptr);
            if (result == -1) {
                if (errno != EINTR) {
                    LOG_ERROR(format_error("select call failed"));
                    throw execution_exception("failed to read child output.");
                }
                // Interrupted by signal
                LOG_DEBUG("select call was interrupted and will be retried.");
                continue;
            }
            if (result == 0) {
                // Read timeout, try again
                continue;
            }

            for (auto& pipe : pipes) {
                if (pipe.descriptor == -1 || !FD_ISSET(pipe.descriptor, &read_set)) {
                    continue;
                }

                // There is data to read
                auto count = read(pipe.descriptor, &pipe.buffer[0], pipe.buffer.size());
                if (count < 0) {
                    if (errno != EINTR) {
                        LOG_ERROR("%1% pipe read failed: %2%.", pipe.name, format_error());
                        throw execution_exception("failed to read child output.");
                    }
                    // Interrupted by signal
                    LOG_DEBUG("%1% pipe read was interrupted and will be retried.", pipe.name);
                    continue;
                }
                if (count == 0) {
                    // Pipe has closed
                    pipe.descriptor = -1;
                    continue;
                }
                // Call the callback
                pipe.buffer.resize(count);
                if (!pipe.callback(pipe.buffer)) {
                    // Callback signaled that we're done
                    return;
                }
            }

            if (input.first != -1 && FD_ISSET(input.first, &write_set)) {
                // Expecting data on input
                auto count = write(input.first, input.second.c_str(), input.second.size());
                if (count < 0) {
                    if (errno != EINTR) {
                        LOG_ERROR("stdin pipe write failed: %1%.", format_error());
                        throw execution_exception("failed to write child input.");
                    }
                    // Interrupted by signal
                    LOG_DEBUG("stdin pipe write was interrupted and will be retried.");
                    continue;
                }
                if (count == 0) {
                    // Pipe has closed
                    input = {};
                    continue;
                }
                // Register written data
                input.second.erase(0, count);
                continue;
            }
        }

        // Should only reach here if the command timed out
        // cppcheck-suppress zerodivcond - http://trac.cppcheck.net/ticket/5402
        throw timeout_exception((boost::format("command timed out after %1% seconds.") % timeout).str(), static_cast<size_t>(child));
    }

    static void exec_child(int in, int out, int err, char const* program, char const** argv, char const** envp)
    {
        // WARNING: this function is called from a vfork'd child
        // Do not modify program state from this function; only call setpgid, dup2, close, execve, and _exit
        // Do not allocate heap memory or throw exceptions
        // The child is sharing the address space of the parent process, so carelessly modifying this
        // function may lead to parent state corruption, memory leaks, and/or total protonic reversal

        // Set the process group; this will be used by the parent if we need to kill the process and its children
        if (setpgid(0, 0) == -1) {
            char const* message = "failed to setpgid.";
            if (write(err, message, strlen(message)) == -1) {
                // Do not care
            }
            return;
        }

        // Redirect stdin
        if (dup2(in, STDIN_FILENO) == -1) {
            char const* message = "failed to redirect child stdin.";
            if (write(err, message, strlen(message)) == -1) {
                // Do not care
            }
            return;
        }

        // Redirect stdout
        if (dup2(out, STDOUT_FILENO) == -1) {
            char const* message = "failed to redirect child stdout.";
            if (write(err, message, strlen(message)) == -1) {
                // Do not care
            }
            return;
        }

        // Redirect stderr
        if (dup2(err, STDERR_FILENO) == -1) {
            char const* message = "failed to redirect child stderr.";
            if (write(err, message, strlen(message)) == -1) {
                // Do not care
            }
            return;
        }

        // Close all open file descriptors above stderr
        for (decltype(get_max_descriptor_limit()) i = (STDERR_FILENO + 1); i < get_max_descriptor_limit(); ++i) {
            close(i);
        }

        // Execute the given program; this should not return if successful
        execve(program, const_cast<char* const*>(argv), const_cast<char* const*>(envp));
    }

    // Helper function that turns a vector of strings into a vector of const cstr pointers
    // This is used to pass arguments and environment to execve
    static vector<char const*> to_exec_arg(vector<string> const* argument, string const* first = nullptr)
    {
        vector<char const*> result;
        result.reserve((argument ? argument->size() : 0) + (first ? 1 : 0) + 1 /* terminating null */);
        if (first) {
            result.push_back(first->c_str());
        }
        if (argument) {
            transform(argument->begin(), argument->end(), back_inserter(result), [](string const& s) { return s.c_str(); });
        }
        // Null terminate the list
        result.push_back(nullptr);
        return result;
    }

    // Helper function that creates a vector of environment variables in the format of key=value
    // Also handles merging of environment and defaulting LC_ALL and LANG to C
    static vector<string> create_environment(map<string, string> const* environment, bool merge)
    {
        vector<string> result;

        // Merge in our current environment, if requested
        if (merge && environ) {
            for (auto var = environ; *var; ++var) {
                // Don't respect LC_ALL or LANG from the parent process
                if (boost::starts_with(*var, "LC_ALL=") || boost::starts_with(*var, "LANG=")) {
                    continue;
                }
                result.emplace_back(*var);
            }
        }

        // Add the given environment
        if (environment) {
            for (auto const& kvp : *environment) {
                result.emplace_back((boost::format("%1%=%2%") % kvp.first % kvp.second).str());
            }
        }

        // Set the locale to C unless specified in the given environment
        if (!environment || environment->count("LC_ALL") == 0) {
            result.emplace_back("LC_ALL=C");
        }
        if (!environment || environment->count("LANG") == 0) {
            result.emplace_back("LANG=C");
        }
        return result;
    }

    static pid_t create_child(int in, int out, int err, char const* program, char const** argv, char const** envp)
    {
        // Fork the child process
        // Note: this uses vfork, which is inherently unsafe (the parent's address space is shared with the child)
        pid_t child = vfork();
        if (child < 0) {
            throw execution_exception(format_error("failed to fork child process"));
        }

        // If this is the parent process, return
        if (child != 0) {
            return child;
        }

        // Exec the child; this only returns if there was a failure
        exec_child(in, out, err, program, argv, envp);

        // If we've reached here, we've failed, so exit the child
        _exit(errno == 0 ? EXIT_FAILURE : errno);
        return -1;
    }

    result execute(
        string const& file,
        vector<string> const* arguments,
        string const* input,
        map<string, string> const* environment,
        function<bool(string&)> const& stdout_callback,
        function<bool(string&)> const& stderr_callback,
        option_set<execution_options> const& options,
        uint32_t timeout)
    {
        // Search for the executable
        string executable = which(file);
        log_execution(executable.empty() ? file : executable, arguments);
        if (executable.empty()) {
            LOG_DEBUG("%1% was not found on the PATH.", file);
            if (options[execution_options::throw_on_nonzero_exit]) {
                throw child_exit_exception("child process returned non-zero exit status.", 127, {}, {});
            }
            return {false, "", "", 127};
        }

        // Create the pipes for stdin/stdout redirection
        int pipes[2];
        if (::pipe(pipes) < 0) {
            throw execution_exception(format_error("failed to allocate pipe for stdin redirection"));
        }
        scoped_descriptor stdin_read(pipes[0]);
        scoped_descriptor stdin_write(pipes[1]);

        if (::pipe(pipes) < 0) {
            throw execution_exception(format_error("failed to allocate pipe for stdout redirection"));
        }
        scoped_descriptor stdout_read(pipes[0]);
        scoped_descriptor stdout_write(pipes[1]);

        // Redirect stderr to stdout, null, or to the pipe to read
        scoped_descriptor stderr_read(-1);
        scoped_descriptor stderr_write(-1);
        scoped_descriptor dev_null(-1);
        int child_stderr = -1;
        if (options[execution_options::redirect_stderr_to_stdout]) {
            child_stderr = stdout_write;
        } else if (options[execution_options::redirect_stderr_to_null]) {
            dev_null = scoped_descriptor(open("/dev/null", O_RDWR));
            child_stderr = dev_null;
        } else {
            if (::pipe(pipes) < 0) {
                throw execution_exception(format_error("failed to allocate pipe for stderr redirection"));
            }
            stderr_read = scoped_descriptor(pipes[0]);
            stderr_write = scoped_descriptor(pipes[1]);
            child_stderr = stderr_write;
        }

        // Allocate the child process arguments and envp *before* creating the child
        auto args = to_exec_arg(arguments, &file);
        auto variables = create_environment(environment, options[execution_options::merge_environment]);
        auto envp = to_exec_arg(&variables);

        // Create the child
        pid_t child = create_child(stdin_read, stdout_write, child_stderr, executable.c_str(), args.data(), envp.data());

        // Close the unused descriptors
        if (!input) {
            stdin_write.release();
        }
        stdin_read.release();
        stdout_write.release();
        stderr_write.release();

        // Define a reaper that is invoked when we exit this scope
        // This ensures that the child won't become a zombie if an exception is thrown
        bool kill_child = true;
        bool success = false;
        bool signaled = false;
        int status = 0;
        scope_exit reaper([&]() {
            if (kill_child) {
                kill(-child, SIGKILL);
            }
            // Wait for the child to exit
            if (waitpid(child, &status, 0) == -1) {
                LOG_DEBUG(format_error("waitpid failed"));
                return;
            }
            if (WIFEXITED(status)) {
                status = static_cast<char>(WEXITSTATUS(status));
                success = status == 0;
                return;
            }
            if (WIFSIGNALED(status)) {
                signaled = true;
                status = static_cast<char>(WTERMSIG(status));
                return;
            }
        });

        // Set up an interval timer for timeouts
        // Note: OSX doesn't implement POSIX per-process timers, so we're stuck with the obsolete POSIX timers API
        scope_exit timer_reset;
        if (timeout) {
            struct sigaction sa = {};
            sa.sa_handler = timer_handler;
            if (sigaction(SIGALRM, &sa, nullptr) == -1) {
                LOG_ERROR(format_error("sigaction failed"));
                throw execution_exception(format_error("failed to setup timer"));
            }

            itimerval timer = {};
            timer.it_value.tv_sec = static_cast<decltype(timer.it_interval.tv_sec)>(timeout);
            if (setitimer(ITIMER_REAL, &timer, nullptr) == -1) {
                LOG_ERROR(format_error("setitimer failed"));
                throw execution_exception(format_error("failed to setup timer"));
            }

            // Set the resource to disable the timer
            timer_reset = scope_exit([&]() {
                itimerval timer = {};
                setitimer(ITIMER_REAL, &timer, nullptr);
                command_timedout = false;
            });
        }

        // This somewhat complicated construct performs the following:
        // Calls a platform-agnostic implementation of processing stdout/stderr data
        // The platform agnostic code calls back into the given lambda to do the actual reading
        // It provides two callbacks of its own to call when there's data available on stdout/stderr
        // We return from the lambda when all data has been read
        string output, error;
        tie(output, error) = process_streams(options[execution_options::trim_output], stdout_callback, stderr_callback, [&](function<bool(string const&)> const& process_stdout, function<bool(string const&)> const& process_stderr) {
            array<pipe, 2> pipes = { {
                pipe("stdout", stdout_read, process_stdout),
                pipe("stderr", stderr_read, process_stderr)
            }};

            pair<scoped_descriptor, string> inpipe;
            if (input) {
                inpipe = {move(stdin_write), *input};
            }

            rw_from_child(child, pipes, move(inpipe), timeout);
        });

        // Close the read pipes
        // If the child hasn't sent all the data yet, this may signal SIGPIPE on next write
        stdout_read.release();
        stderr_read.release();

        // Wait for the child to exit
        kill_child = false;
        reaper.invoke();

        if (signaled) {
            LOG_DEBUG("process was signaled with signal %1%.", status);
        } else {
            LOG_DEBUG("process exited with status code %1%.", status);
        }

        // Throw exception if needed
        if (!success) {
            if (!signaled && status != 0 && options[execution_options::throw_on_nonzero_exit]) {
                throw child_exit_exception((boost::format("child process returned non-zero exit status (%1%).") % status).str(), status, move(output), move(error));
            }
            if (signaled && options[execution_options::throw_on_signal]) {
                throw child_signal_exception((boost::format("child process was terminated by signal (%1%).") % status).str(), status, move(output), move(error));
            }
        }
        return {success, move(output), move(error), status};
    }

}}  // namespace leatherman::execution
