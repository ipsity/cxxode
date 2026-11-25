#include "platform/process.hpp"
#include "base/utf.hpp"

#include <mutex>
#include <thread>

namespace ode {
namespace {

class handle {
public:
    explicit handle(HANDLE h = nullptr) : h_(h) {}
    ~handle() { reset(); }
    handle(const handle&) = delete;
    handle& operator=(const handle&) = delete;
    HANDLE get() const { return h_; }
    explicit operator bool() const { return h_ && h_ != INVALID_HANDLE_VALUE; }
    void reset(HANDLE h = nullptr) {
        if (*this) CloseHandle(h_);
        h_ = h;
    }
private:
    HANDLE h_;
};

std::wstring quote(std::wstring_view argument) {
    if (!argument.empty() && argument.find_first_of(L" \t\"") == std::wstring_view::npos) return std::wstring(argument);
    std::wstring result(1, L'"');
    std::size_t slashes = 0;
    for (const auto c : argument) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result += c;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    result += L'"';
    return result;
}

std::string error_text(std::string_view what) {
    return std::string(what) + " (Windows error " + std::to_string(GetLastError()) + ")";
}

} // namespace

std::optional<std::filesystem::path> find_on_path(std::string_view name) {
    wchar_t buffer[MAX_PATH * 4];
    const auto wide = widen(name);
    const DWORD n = SearchPathW(nullptr, wide.c_str(), L".exe", static_cast<DWORD>(std::size(buffer)), buffer, nullptr);
    if (n == 0 || n >= std::size(buffer)) return std::nullopt;
    return std::filesystem::path(buffer);
}

process_result run_process(const process_request& request, cancel_token& cancel,
                           const std::function<void(std::string_view)>& on_output) {
    using clock = std::chrono::steady_clock;
    const auto began = clock::now();
    process_result result;

    SECURITY_ATTRIBUTES inherit{sizeof(inherit), nullptr, TRUE};
    HANDLE read_raw = nullptr, write_raw = nullptr;
    if (!CreatePipe(&read_raw, &write_raw, &inherit, 65536)) {
        result.error = error_text("CreatePipe");
        return result;
    }
    handle read_end(read_raw), write_end(write_raw);
    SetHandleInformation(read_end.get(), HANDLE_FLAG_INHERIT, 0);

    // Setup stdin: pipe if we have data to send, otherwise NUL
    handle stdin_read, stdin_write;
    if (!request.stdin_data.empty()) {
        HANDLE stdin_r = nullptr, stdin_w = nullptr;
        if (!CreatePipe(&stdin_r, &stdin_w, &inherit, 0)) {
            result.error = error_text("CreatePipe for stdin");
            return result;
        }
        stdin_read.reset(stdin_r);
        stdin_write.reset(stdin_w);
        SetHandleInformation(stdin_write.get(), HANDLE_FLAG_INHERIT, 0);
    } else {
        stdin_read.reset(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherit, OPEN_EXISTING, 0, nullptr));
    }

    handle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job || !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        result.error = error_text("Job object setup");
        return result;
    }

    std::wstring command = quote(request.program.wstring());
    for (const auto& arg : request.arguments) command += L' ' + quote(widen(arg));
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = stdin_read.get();
    startup.hStdOutput = write_end.get();
    startup.hStdError = write_end.get();
    PROCESS_INFORMATION child{};
    const auto directory = request.directory.wstring();
    if (!CreateProcessW(request.program.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT, nullptr,
                        directory.empty() ? nullptr : directory.c_str(), &startup, &child)) {
        result.error = error_text("CreateProcessW");
        return result;
    }
    handle process(child.hProcess), thread(child.hThread);
    // Assign before the first instruction runs so no descendant escapes the job.
    AssignProcessToJobObject(job.get(), process.get());
    ResumeThread(thread.get());
    thread.reset();
    write_end.reset();

    // Write stdin data if provided, then close the write end
    if (!request.stdin_data.empty()) {
        DWORD written = 0;
        WriteFile(stdin_write.get(), request.stdin_data.data(), static_cast<DWORD>(request.stdin_data.size()), &written, nullptr);
        stdin_write.reset();  // Close stdin so the child sees EOF
    }

    std::mutex mutex;
    std::thread reader([&] {
        char buffer[8192];
        DWORD received = 0;
        while (ReadFile(read_end.get(), buffer, sizeof(buffer), &received, nullptr) && received) {
            std::lock_guard lock(mutex);
            result.output.append(buffer, received);
            if (on_output) on_output({buffer, received});
        }
    });

    auto leader_exit = clock::time_point::max();
    bool killed = false;
    while (true) {
        const auto wait = WaitForSingleObject(process.get(), 25);
        const auto now = clock::now();
        if (wait == WAIT_OBJECT_0 && leader_exit == clock::time_point::max()) {
            leader_exit = now;
            DWORD code = 0;
            if (GetExitCodeProcess(process.get(), &code)) result.exit_code = static_cast<int>(code);
        }
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION accounting{};
        QueryInformationJobObject(job.get(), JobObjectBasicAccountingInformation, &accounting, sizeof(accounting), nullptr);
        if (leader_exit != clock::time_point::max() && accounting.ActiveProcesses == 0) break;
        const bool cancel_now = cancel.cancelled();
        const bool timeout = now - began >= request.timeout;
        const bool lingering = leader_exit != clock::time_point::max() && now - leader_exit >= std::chrono::milliseconds(200);
        if (!killed && (cancel_now || timeout || lingering)) {
            result.cancelled = cancel_now && leader_exit == clock::time_point::max();
            result.timed_out = timeout && !cancel_now && leader_exit == clock::time_point::max();
            TerminateJobObject(job.get(), 1);
            killed = true;
        }
        if (killed && leader_exit != clock::time_point::max()) break;
    }
    // With the job empty every write end is closed and the reader sees EOF.
    TerminateJobObject(job.get(), 1);
    reader.join();
    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - began);
    return result;
}

} // namespace ode
