#include "platform/interactive_process.hpp"
#include "platform/win_util.hpp"

#include <stdexcept>
#include <thread>

namespace ode {

interactive_process::interactive_process(const std::string& command, const std::vector<std::string>& args,
                                         const options& opts) {
    // A server may be named by a bare command such as "npx". SearchPathW finds it, but a
    // .cmd or .bat cannot be the image of CreateProcessW, so those go through cmd.exe.
    const auto found = fs::path(command).is_absolute() ? fs::path(command) : find_on_path(command);
    if (!found) throw std::runtime_error("Cannot find " + command + " on PATH");
    const auto extension = found->extension().wstring();
    const bool script = extension == L".cmd" || extension == L".bat";
    fs::path program = *found;
    std::vector<std::string> launch_args = args;
    if (script) {
        const auto shell = find_on_path("cmd.exe");
        if (!shell) throw std::runtime_error("Cannot find cmd.exe");
        program = *shell;
        const std::vector<std::string> prefix = {"/d", "/s", "/c", narrow(found->wstring())};
        launch_args.insert(launch_args.begin(), prefix.begin(), prefix.end());
    }

    // Force quotes on every argument. The usual quoting leaves & and | bare, and MCP servers are
    // routinely launched through cmd.exe, which treats those as command separators.
    std::wstring cmdline = win::quote(program.wstring(), true);
    for (const auto& arg : launch_args) cmdline += L' ' + win::quote(widen(arg), true);

    // The child inherits the block, so a custom environment has to be a complete one.
    std::wstring environment;
    if (!opts.env.empty()) {
        for (wchar_t** v = _wenviron; v && *v; ++v) environment.append(*v).push_back(L'\0');
        for (const auto& [key, value] : opts.env) environment += widen(key + "=" + value) + L'\0';
        environment.push_back(L'\0');
    }

    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE stdin_read = nullptr, stdin_write = nullptr;
    HANDLE stdout_read = nullptr, stdout_write = nullptr;
    HANDLE stderr_read = nullptr, stderr_write = nullptr;
    const bool pipes = CreatePipe(&stdin_read, &stdin_write, &inherit, 65536) &&
                       SetHandleInformation(stdin_write, HANDLE_FLAG_INHERIT, 0) &&
                       CreatePipe(&stdout_read, &stdout_write, &inherit, 65536) &&
                       SetHandleInformation(stdout_read, HANDLE_FLAG_INHERIT, 0) &&
                       CreatePipe(&stderr_read, &stderr_write, &inherit, 65536) &&
                       SetHandleInformation(stderr_read, HANDLE_FLAG_INHERIT, 0);
    win::handle child_stdin(stdin_read), own_stdin(stdin_write);
    win::handle own_stdout(stdout_read), child_stdout(stdout_write);
    win::handle own_stderr(stderr_read), child_stderr(stderr_write);
    if (!pipes) throw std::runtime_error("CreatePipe failed");

    // Kills the whole tree, including anything the server itself spawns, when ode exits.
    job_.reset(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job_ || !SetInformationJobObject(job_.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        throw std::runtime_error("Job object setup failed");
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdin_read;
    startup.hStdOutput = stdout_write;
    startup.hStdError = stderr_write;
    PROCESS_INFORMATION child{};
    const auto directory = opts.working_dir.wstring();
    if (!CreateProcessW(program.c_str(), cmdline.data(), nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT,
                        environment.empty() ? nullptr : environment.data(),
                        directory.empty() ? nullptr : directory.c_str(), &startup, &child)) {
        throw std::runtime_error("CreateProcessW failed for " + command);
    }
    process_.reset(child.hProcess);
    pid_ = child.dwProcessId;
    win::handle thread(child.hThread);
    // Assign before the first instruction runs so no descendant escapes the job.
    // Resume even on failure: a suspended child would otherwise outlive the exception.
    const bool assigned = AssignProcessToJobObject(job_.get(), process_.get());
    ResumeThread(thread.get());
    if (!assigned) {
        TerminateProcess(process_.get(), 1);
        throw std::runtime_error("AssignProcessToJobObject failed for " + command);
    }
    stdin_write_ = own_stdin.release();
    stdout_read_ = own_stdout.release();
    stderr_read_ = own_stderr.release();

    // Nobody reads stderr yet, and a server that logs to it would stall once the pipe fills.
    std::thread([read = stderr_read_] {
        char buffer[4096];
        DWORD got = 0;
        while (ReadFile(read, buffer, sizeof(buffer), &got, nullptr) && got) {}
        CloseHandle(read);
    }).detach();
    stderr_read_ = nullptr;
}

interactive_process::~interactive_process() { terminate(); }

bool interactive_process::is_running() const {
    if (!process_) return false;
    return WaitForSingleObject(process_.get(), 0) == WAIT_TIMEOUT;
}

std::size_t interactive_process::write_stdin(const void* data, std::size_t size) {
    if (!stdin_write_) return 0;
    DWORD written = 0;
    if (!WriteFile(stdin_write_, data, static_cast<DWORD>(size), &written, nullptr)) return 0;
    return written;
}

void interactive_process::close_stdin() {
    if (stdin_write_) CloseHandle(stdin_write_);
    stdin_write_ = nullptr;
}

std::size_t interactive_process::read_stdout(void* buffer, std::size_t size, std::chrono::milliseconds timeout) {
    if (!stdout_read_) return 0;
    // An anonymous pipe handle is not a waitable object, so WaitForSingleObject never signals
    // on arriving data. PeekNamedPipe is the only way to poll one without blocking.
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        DWORD available = 0;
        if (!PeekNamedPipe(stdout_read_, nullptr, 0, nullptr, &available, nullptr)) return 0;  // pipe closed
        if (available > 0) break;
        if (std::chrono::steady_clock::now() >= deadline) return 0;
        Sleep(5);
    }
    DWORD read = 0;
    if (!ReadFile(stdout_read_, buffer, static_cast<DWORD>(size), &read, nullptr)) return 0;
    return read;
}

std::size_t interactive_process::read_stderr(void* buffer, std::size_t size) {
    if (!stderr_read_) return 0;
    DWORD read = 0;
    if (!ReadFile(stderr_read_, buffer, static_cast<DWORD>(size), &read, nullptr)) return 0;
    return read;
}

void interactive_process::terminate() {
    if (process_) {
        TerminateProcess(process_.get(), 1);
        process_.reset();
    }
    job_.reset();
    if (stdin_write_) CloseHandle(stdin_write_);
    if (stdout_read_) CloseHandle(stdout_read_);
    stdin_write_ = stdout_read_ = nullptr;
    // stderr_read_ is owned by the drain thread, which closes it when the pipe breaks.
}

} // namespace ode
