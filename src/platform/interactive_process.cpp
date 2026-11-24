#include "platform/interactive_process.hpp"

#ifdef _WIN32
#include <stdexcept>

namespace ode {

interactive_process::interactive_process(const std::string& command, const std::vector<std::string>& args,
                                         const options& opts) {
    // Build command line
    std::wstring cmdline;
    cmdline += L"\"";
    for (char c : command) cmdline += static_cast<wchar_t>(c);
    cmdline += L"\"";
    for (const auto& arg : args) {
        cmdline += L" \"";
        for (char c : arg) cmdline += static_cast<wchar_t>(c);
        cmdline += L"\"";
    }

    // Create pipes
    SECURITY_ATTRIBUTES sa{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
    HANDLE stdin_read, stdout_write, stderr_write;

    if (!CreatePipe(&stdin_read, &stdin_write_, &sa, 0) ||
        !SetHandleInformation(stdin_write_, HANDLE_FLAG_INHERIT, 0) ||
        !CreatePipe(&stdout_read_, &stdout_write, &sa, 0) ||
        !SetHandleInformation(stdout_read_, HANDLE_FLAG_INHERIT, 0) ||
        !CreatePipe(&stderr_read_, &stderr_write, &sa, 0) ||
        !SetHandleInformation(stderr_read_, HANDLE_FLAG_INHERIT, 0)) {
        throw std::runtime_error("Failed to create pipes");
    }

    // Start process
    STARTUPINFOW si{sizeof(STARTUPINFOW)};
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdInput = stdin_read;
    si.hStdOutput = stdout_write;
    si.hStdError = stderr_write;

    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(nullptr, cmdline.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                        nullptr, nullptr, &si, &pi)) {
        CloseHandle(stdin_read);
        CloseHandle(stdout_write);
        CloseHandle(stderr_write);
        throw std::runtime_error("Failed to start process");
    }

    process_ = pi.hProcess;
    pid_ = pi.dwProcessId;
    CloseHandle(pi.hThread);
    CloseHandle(stdin_read);
    CloseHandle(stdout_write);
    CloseHandle(stderr_write);
}

interactive_process::~interactive_process() {
    if (process_) {
        TerminateProcess(process_, 1);
        CloseHandle(process_);
    }
    if (stdin_write_) CloseHandle(stdin_write_);
    if (stdout_read_) CloseHandle(stdout_read_);
    if (stderr_read_) CloseHandle(stderr_read_);
}

bool interactive_process::is_running() const {
    if (!process_) return false;
    DWORD exit_code = 0;
    GetExitCodeProcess(process_, &exit_code);
    return exit_code == STILL_ACTIVE;
}

std::size_t interactive_process::write_stdin(const void* data, std::size_t size) {
    if (!stdin_write_) return 0;
    DWORD written = 0;
    WriteFile(stdin_write_, data, static_cast<DWORD>(size), &written, nullptr);
    return written;
}

std::size_t interactive_process::read_stdout(void* buffer, std::size_t size) {
    if (!stdout_read_) return 0;
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
        TerminateProcess(process_, 1);
        CloseHandle(process_);
        process_ = nullptr;
    }
}

} // namespace ode

#endif
