#include "platform/process.hpp"

#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <utility>

namespace lay::platform {
namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

class Handle {
public:
    explicit Handle(HANDLE value = nullptr) : value_(value) {}
    ~Handle() { reset(); }
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    HANDLE get() const { return value_; }
    explicit operator bool() const { return value_ && value_ != INVALID_HANDLE_VALUE; }
    void reset(HANDLE value = nullptr) {
        if (*this) {
            CloseHandle(value_);
        }
        value_ = value;
    }
private:
    HANDLE value_;
};

struct Pipe {
    Handle read;
    Handle write;
    Capture output;
    bool ended = false;

    bool create() {
        SECURITY_ATTRIBUTES attributes{sizeof(attributes), nullptr, TRUE};
        HANDLE input = nullptr, output_handle = nullptr;
        if (!CreatePipe(&input, &output_handle, &attributes, 65536)) {
            return false;
        }
        read.reset(input);
        write.reset(output_handle);
        return SetHandleInformation(read.get(), HANDLE_FLAG_INHERIT, 0) != 0;
    }

    bool drain(bool& progress) {
        if (ended) {
            return true;
        }
        char buffer[16384];
        for (int block = 0; block < 4; ++block) {
            DWORD available = 0, received = 0;
            if (!PeekNamedPipe(read.get(), nullptr, 0, nullptr, &available, nullptr) ||
                (available && !ReadFile(read.get(), buffer,
                    std::min<DWORD>(available, sizeof(buffer)), &received, nullptr))) {
                const auto error = GetLastError();
                ended = error == ERROR_BROKEN_PIPE || error == ERROR_PIPE_NOT_CONNECTED;
                return ended;
            }
            if (!received) {
                return true;
            }
            output.append({buffer, received});
            progress = true;
        }
        return true;
    }
};

std::wstring quote(std::wstring_view argument) {
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

struct Attributes {
    std::vector<std::max_align_t> memory;
    LPPROC_THREAD_ATTRIBUTE_LIST list = nullptr;

    ~Attributes() { if (list) DeleteProcThreadAttributeList(list); }

    bool init() {
        SIZE_T bytes = 0;
        InitializeProcThreadAttributeList(nullptr, 2, 0, &bytes);
        if (!bytes) {
            return false;
        }
        memory.resize((bytes + sizeof(std::max_align_t) - 1) / sizeof(std::max_align_t));
        auto* storage = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(memory.data());
        if (!InitializeProcThreadAttributeList(storage, 2, 0, &bytes)) {
            return false;
        }
        list = storage;
        return true;
    }
};

std::string system_error(std::string_view operation, DWORD error = GetLastError()) {
    return std::string(operation) + " (Windows error " + std::to_string(error) + ")";
}

} // namespace

ProcessResult run_process(const ProcessInput& input, const StopRequested& stop) {
    const auto began = Clock::now();
    ProcessResult result;
    const auto startup_failure = [&](std::string why) {
        result.diagnostic = std::move(why);
        return result;
    };
    Pipe out, err;
    if (!out.create() || !err.create()) {
        return startup_failure(system_error("Could not create output pipes"));
    }
    SECURITY_ATTRIBUTES inherited{sizeof(inherited), nullptr, TRUE};
    const Handle stdin_null(CreateFileW(L"NUL", GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                        &inherited, OPEN_EXISTING, 0, nullptr));
    const Handle job(CreateJobObjectW(nullptr, nullptr));
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
    limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!stdin_null || !job ||
        !SetInformationJobObject(job.get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits))) {
        return startup_failure(system_error("Could not configure the child job"));
    }

    Attributes attributes;
    HANDLE inherited_handles[]{stdin_null.get(), out.write.get(), err.write.get()};
    HANDLE job_handles[]{job.get()};
    constexpr DWORD_PTR job_list_attribute = 0x0002000d;
    if (!attributes.init() ||
        !UpdateProcThreadAttribute(attributes.list, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   inherited_handles, sizeof(inherited_handles), nullptr, nullptr) ||
        !UpdateProcThreadAttribute(attributes.list, 0, job_list_attribute,
                                   job_handles, sizeof(job_handles), nullptr, nullptr)) {
        return startup_failure(system_error("Could not restrict inherited handles and bind the child to its job"));
    }
    auto command = quote(input.executable.native());
    for (const auto& arg : input.arguments) {
        command += L' ';
        command += quote(path_from_utf8(arg).wstring());
    }
    if (command.size() >= 32767) {
        return startup_failure("Encoded Windows command line exceeds 32766 characters");
    }
    std::wstring environment;
    for (const auto& item : input.environment) {
        environment += path_from_utf8(item).wstring();
        environment += L'\0';
    }
    if (environment.empty()) {
        environment += L'\0';
    }
    environment += L'\0';
    const auto directory = input.directory.current_path();
    if (stop()) {
        result.status = ProcessStatus::cancelled;
        return result;
    }
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = stdin_null.get();
    startup.StartupInfo.hStdOutput = out.write.get();
    startup.StartupInfo.hStdError = err.write.get();
    startup.lpAttributeList = attributes.list;
    PROCESS_INFORMATION child{};
    if (!CreateProcessW(input.executable.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT |
                        CREATE_UNICODE_ENVIRONMENT, environment.data(), directory.c_str(),
                        &startup.StartupInfo, &child)) {
        return startup_failure(system_error("CreateProcessW failed"));
    }
    const Handle process(child.hProcess);
    Handle thread(child.hThread);
    out.write.reset();
    err.write.reset();
    if (ResumeThread(thread.get()) == static_cast<DWORD>(-1)) {
        return startup_failure(system_error("Could not start the suspended child"));
    }
    thread.reset();

    auto leader_done = Clock::time_point::max();
    auto terminating = Clock::time_point::max();
    while (true) {
        bool progress = false;
        const auto now = Clock::now();
        JOBOBJECT_BASIC_ACCOUNTING_INFORMATION jobs{};
        const auto leader = WaitForSingleObject(process.get(), 0);
        if (!out.drain(progress) || !err.drain(progress) || leader == WAIT_FAILED ||
            !QueryInformationJobObject(job.get(), JobObjectBasicAccountingInformation,
                                       &jobs, sizeof(jobs), nullptr)) {
            result.status = ProcessStatus::unknown;
            result.diagnostic = system_error("Could not observe child process or pipes");
            break;
        }
        if (leader == WAIT_OBJECT_0 && leader_done == Clock::time_point::max()) {
            leader_done = now;
            DWORD code = 0;
            if (!GetExitCodeProcess(process.get(), &code)) {
                result.status = ProcessStatus::unknown;
                result.diagnostic = system_error("Could not read child exit status");
                break;
            }
            result.code = code;
            if (terminating == Clock::time_point::max()) {
                result.status = ProcessStatus::exited;
            }
        }
        if (jobs.ActiveProcesses == 0 && leader == WAIT_OBJECT_0 && out.ended && err.ended) {
            break;
        }
        if (terminating == Clock::time_point::max() &&
            (stop() || now - began >= input.timeout ||
             (leader == WAIT_OBJECT_0 && now - leader_done >= 180ms))) {
            result.descendants_terminated = leader == WAIT_OBJECT_0 && jobs.ActiveProcesses != 0;
            result.status = stop() ? ProcessStatus::cancelled :
                leader == WAIT_OBJECT_0 ? ProcessStatus::exited : ProcessStatus::timed_out;
            if (!TerminateJobObject(job.get(), 1)) {
                result.status = ProcessStatus::unknown;
                result.diagnostic = system_error("Could not terminate the child job");
                break;
            }
            terminating = now;
        }
        if (terminating != Clock::time_point::max() && now - terminating >= 2500ms) {
            result.status = ProcessStatus::unknown;
            result.diagnostic = "Job or output pipes remained active after termination";
            break;
        }
        if (!progress) {
            Sleep(1);
        }
    }
    result.elapsed_ms = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - began).count());
    result.stdout_text = out.output.finish();
    result.stderr_text = err.output.finish();
    return result;
}

} // namespace lay::platform
