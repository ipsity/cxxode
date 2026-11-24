#include "platform/process.hpp"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <fcntl.h>
#include <linux/close_range.h>
#include <poll.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

namespace lay::platform {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

class File {
public:
    ~File() { reset(); }
    File() = default;
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    int get() const { return fd_; }
    void reset(int fd = -1) {
        if (fd_ != -1) {
            ::close(fd_);
        }
        fd_ = fd;
    }
    bool move_above_stdio() {
        if (fd_ >= 3) {
            return true;
        }
        if (fd_ == -1) {
            return false;
        }
        const auto duplicate = ::fcntl(fd_, F_DUPFD_CLOEXEC, 3);
        if (duplicate == -1) {
            return false;
        }
        reset(duplicate);
        return true;
    }
private:
    int fd_ = -1;
};

struct Pipe {
    File read;
    File write;
    Capture output;

    bool create() {
        int ends[2]{};
        if (::pipe2(ends, O_CLOEXEC) != 0) {
            return false;
        }
        read.reset(ends[0]);
        write.reset(ends[1]);
        return read.move_above_stdio() && write.move_above_stdio() &&
            ::fcntl(read.get(), F_SETFL, O_NONBLOCK) == 0;
    }

    bool drain(bool& progress) {
        if (read.get() == -1) {
            return true;
        }
        char buffer[16384];
        for (int block = 0; block < 4; ++block) {
            const auto count = ::read(read.get(), buffer, sizeof(buffer));
            if (count == 0) {
                read.reset();
                return true;
            }
            if (count < 0) {
                return errno == EAGAIN || errno == EINTR;
            }
            output.append({buffer, static_cast<std::size_t>(count)});
            progress = true;
        }
        return true;
    }
};

struct Subreaper {
    int previous = 0;
    bool active = false;
    ~Subreaper() {
        if (active) {
            ::prctl(PR_SET_CHILD_SUBREAPER, previous);
        }
    }
    bool enable() {
        active = ::prctl(PR_GET_CHILD_SUBREAPER, &previous) == 0 &&
            ::prctl(PR_SET_CHILD_SUBREAPER, 1) == 0;
        return active;
    }
};

struct Cleanup {
    pid_t leader;
    bool armed = true;
    ~Cleanup() {
        if (armed) {
            ::kill(-leader, SIGKILL);
            ::waitpid(leader, nullptr, WNOHANG);
        }
    }
};

std::vector<char*> pointers(TextList& storage) {
    std::vector<char*> out;
    out.reserve(storage.size() + 1);
    for (auto& item : storage) {
        out.push_back(item.data());
    }
    out.push_back(nullptr);
    return out;
}

std::string system_error(std::string_view operation, int code = errno) {
    return std::string(operation) + ": " + std::strerror(code);
}

[[noreturn]] void child_failure(int descriptor, int code) noexcept {
    while (::write(descriptor, &code, sizeof(code)) < 0 && errno == EINTR) {}
    _exit(127);
}

} // namespace

ProcessResult run_process(const ProcessInput& input, const StopRequested& stop) {
    const auto began = Clock::now();
    ProcessResult result;
    const auto startup_failure = [&](std::string why) {
        result.diagnostic = std::move(why);
        return result;
    };
    Pipe out, err, launch;
    File stdin_null;
    stdin_null.reset(::open("/dev/null", O_RDONLY | O_CLOEXEC));
    Subreaper subreaper;
    if (!stdin_null.move_above_stdio() || !out.create() || !err.create() || !launch.create() ||
        !subreaper.enable()) {
        return startup_failure(system_error("Could not prepare child process group and pipes"));
    }
    auto arguments = input.arguments;
    arguments.insert(arguments.begin(), input.executable.native());
    auto environment = input.environment;
    auto argv = pointers(arguments);
    auto envp = pointers(environment);
    const auto* executable = input.executable.c_str();
    const auto directory = static_cast<int>(input.directory.descriptor());
    const auto host = ::getpid();
    if (stop()) {
        result.status = ProcessStatus::cancelled;
        return result;
    }
    const auto child = ::fork();
    if (child == -1) {
        return startup_failure(system_error("fork failed"));
    }
    if (child == 0) {
        const auto error_pipe = launch.write.get();
        if (::prctl(PR_SET_PDEATHSIG, SIGKILL) != 0 || ::getppid() != host) {
            _exit(127);
        }
        if (::setpgid(0, 0) != 0 || ::fchdir(directory) != 0 ||
            ::dup2(stdin_null.get(), STDIN_FILENO) == -1 ||
            ::dup2(out.write.get(), STDOUT_FILENO) == -1 ||
            ::dup2(err.write.get(), STDERR_FILENO) == -1 ||
            ::syscall(SYS_close_range, 3u, ~0u, CLOSE_RANGE_CLOEXEC) != 0) {
            child_failure(error_pipe, errno);
        }
        ::execve(executable, argv.data(), envp.data());
        child_failure(error_pipe, errno);
    }

    Cleanup cleanup{child};
    // This races only with the child's identical setpgid and makes the group
    // available to cancellation even before execve.
    ::setpgid(child, child);
    out.write.reset();
    err.write.reset();
    launch.write.reset();
    auto leader_done = Clock::time_point::max();
    auto terminating = Clock::time_point::max();
    bool forced = false;
    int exec_error = 0;

    while (true) {
        bool progress = false;
        const auto now = Clock::now();
        if (launch.read.get() != -1) {
            const auto count = ::read(launch.read.get(), &exec_error, sizeof(exec_error));
            if (count >= 0) {
                launch.read.reset();
            } else if (errno != EAGAIN && errno != EINTR) {
                result.status = ProcessStatus::unknown;
                result.diagnostic = system_error("Could not observe execve");
                break;
            }
        }
        while (true) {
            int status = 0;
            const auto pid = ::waitpid(-child, &status, WNOHANG);
            if (pid < 0 && errno == EINTR) {
                continue;
            }
            if (pid <= 0) {
                break;
            }
            if (pid == child) {
                leader_done = now;
                if (terminating == Clock::time_point::max()) {
                    result.status = WIFEXITED(status) ? ProcessStatus::exited : ProcessStatus::signaled;
                }
                result.code = WIFEXITED(status) ? WEXITSTATUS(status) : WTERMSIG(status);
            }
        }
        if (!out.drain(progress) || !err.drain(progress)) {
            result.status = ProcessStatus::unknown;
            result.diagnostic = system_error("Could not read child output");
            break;
        }
        const bool finished = leader_done != Clock::time_point::max();
        const bool group_active = ::kill(-child, 0) == 0 || errno != ESRCH;
        if (finished && !group_active && out.read.get() == -1 && err.read.get() == -1 &&
            launch.read.get() == -1) {
            cleanup.armed = false;
            break;
        }
        const bool cancelled = stop();
        if (terminating == Clock::time_point::max() &&
            (cancelled || now - began >= input.timeout || (finished && now - leader_done >= 180ms))) {
            result.descendants_terminated = finished && group_active;
            result.status = cancelled ? ProcessStatus::cancelled : finished ? result.status : ProcessStatus::timed_out;
            ::kill(-child, SIGTERM);
            terminating = now;
        }
        if (terminating != Clock::time_point::max() && now - terminating >= 2s && !forced) {
            ::kill(-child, SIGKILL);
            forced = true;
        }
        if (terminating != Clock::time_point::max() && now - terminating >= 2750ms) {
            result.status = ProcessStatus::unknown;
            result.diagnostic = "Process group or pipes remained active after termination";
            break;
        }
        if (!progress) {
            pollfd pipes[]{{out.read.get(), POLLIN, 0}, {err.read.get(), POLLIN, 0},
                           {launch.read.get(), POLLIN, 0}};
            ::poll(pipes, 3, 10);
        }
    }
    if (exec_error && result.status == ProcessStatus::exited) {
        result.status = ProcessStatus::not_started;
        result.code = 0;
        result.diagnostic = system_error("Could not start the executable", exec_error);
    }
    result.elapsed_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - began).count());
    result.stdout_text = out.output.finish();
    result.stderr_text = err.output.finish();
    return result;
}

} // namespace lay::platform
