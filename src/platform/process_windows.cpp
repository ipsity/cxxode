#include "platform/process.hpp"
#include "platform/win_util.hpp"

#include <algorithm>
#include <map>
#include <memory>
#include <thread>
#include <type_traits>

namespace ode {
namespace {

using win::handle;
using win::quote;

std::string error_text(std::string_view what) {
    return std::string(what) + " (Windows error " + std::to_string(GetLastError()) + ")";
}

std::wstring environment_block(const process_request& request) {
    if (request.env.empty()) return {};
    const auto less = [](const std::wstring& a, const std::wstring& b) {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    };
    std::map<std::wstring, std::wstring, decltype(less)> values(less);
    const std::unique_ptr<wchar_t, decltype(&FreeEnvironmentStringsW)> inherited(
        GetEnvironmentStringsW(), FreeEnvironmentStringsW);
    for (auto entry = inherited.get(); entry && *entry;) {
        const std::wstring_view line(entry);
        if (const auto eq = line.find(L'=', 1); eq != std::wstring_view::npos) {
            values.emplace(line.substr(0, eq), line.substr(eq + 1));
        }
        entry += line.size() + 1;
    }
    for (const auto& [key, value] : request.env) values[widen(key)] = widen(value);
    std::wstring block;
    for (const auto& [key, value] : values) block += key + L'=' + value + L'\0';
    block.push_back(L'\0');
    return block;
}

} // namespace

std::optional<std::filesystem::path> find_on_path(std::string_view name) {
    wchar_t buffer[MAX_PATH * 4];
    const auto wide = widen(name);
    const auto search = [&](const wchar_t* ext) -> std::optional<std::filesystem::path> {
        const DWORD n = SearchPathW(nullptr, wide.c_str(), ext, static_cast<DWORD>(std::size(buffer)), buffer, nullptr);
        if (n == 0 || n >= std::size(buffer)) return std::nullopt;
        return std::filesystem::path(buffer);
    };
    // A bare command such as "npx" is backed by npx.cmd on Windows, not npx.exe, so a shell
    // tries each PATHEXT entry. Skip that when the caller already gave an extension.
    if (std::filesystem::path(wide).has_extension()) return search(nullptr);
    for (const wchar_t* ext : {L".com", L".exe", L".bat", L".cmd"}) {
        if (auto found = search(ext)) return found;
    }
    return std::nullopt;
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

    handle stderr_read, stderr_write;
    if (request.separate_stderr) {
        if (!CreatePipe(&read_raw, &write_raw, &inherit, 65536)) {
            result.error = error_text("CreatePipe for stderr");
            return result;
        }
        stderr_read.reset(read_raw);
        stderr_write.reset(write_raw);
        SetHandleInformation(stderr_read.get(), HANDLE_FLAG_INHERIT, 0);
    }

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
    if (!request.command_tail.empty()) command += L' ' + widen(request.command_tail);
    auto environment = environment_block(request);
    STARTUPINFOEXW startup{};
    auto& stdio = startup.StartupInfo;
    stdio.cb = sizeof(startup);
    stdio.dwFlags = STARTF_USESTDHANDLES;
    stdio.hStdInput = stdin_read.get();
    stdio.hStdOutput = write_end.get();
    stdio.hStdError = stderr_write ? stderr_write.get() : write_end.get();
    // Concurrent processes must not inherit each other's pipe ends and keep a reader waiting for EOF.
    HANDLE handles[]{stdio.hStdInput, stdio.hStdOutput, stdio.hStdError};
    SIZE_T attribute_bytes = 0;
    InitializeProcThreadAttributeList(nullptr, 1, 0, &attribute_bytes);
    std::vector<unsigned char> attribute_buffer(attribute_bytes);
    startup.lpAttributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(attribute_buffer.data());
    if (!InitializeProcThreadAttributeList(startup.lpAttributeList, 1, 0, &attribute_bytes)) {
        result.error = error_text("InitializeProcThreadAttributeList");
        return result;
    }
    const std::unique_ptr<std::remove_pointer_t<LPPROC_THREAD_ATTRIBUTE_LIST>, decltype(&DeleteProcThreadAttributeList)>
        attributes(startup.lpAttributeList, DeleteProcThreadAttributeList);
    if (!UpdateProcThreadAttribute(attributes.get(), 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
                                   handles, sizeof(HANDLE) * (stderr_write ? 3 : 2), nullptr, nullptr)) {
        result.error = error_text("UpdateProcThreadAttribute");
        return result;
    }
    PROCESS_INFORMATION child{};
    const auto directory = request.directory.wstring();
    if (!CreateProcessW(request.program.c_str(), command.data(), nullptr, nullptr, TRUE,
                        CREATE_SUSPENDED | CREATE_NO_WINDOW | CREATE_UNICODE_ENVIRONMENT | EXTENDED_STARTUPINFO_PRESENT,
                        environment.empty() ? nullptr : environment.data(),
                        directory.empty() ? nullptr : directory.c_str(), &stdio, &child)) {
        result.error = error_text("CreateProcessW");
        return result;
    }
    handle process(child.hProcess), thread(child.hThread);
    // Assign before the first instruction runs so no descendant escapes the job.
    if (!AssignProcessToJobObject(job.get(), process.get())) {
        result.error = error_text("AssignProcessToJobObject");
        TerminateProcess(process.get(), 1);
        return result;
    }
    ResumeThread(thread.get());
    thread.reset();
    write_end.reset();
    stderr_write.reset();
    stdin_read.reset();

    std::thread reader([&] {
        char buffer[8192];
        DWORD received = 0;
        while (ReadFile(read_end.get(), buffer, sizeof(buffer), &received, nullptr) && received) {
            result.output.append(buffer, received);
            if (on_output) on_output({buffer, received});
        }
    });
    std::thread error_reader;
    if (stderr_read) {
        error_reader = std::thread([&] {
            char buffer[8192];
            DWORD received = 0;
            while (ReadFile(stderr_read.get(), buffer, sizeof(buffer), &received, nullptr) && received)
                result.stderr_output.append(buffer, received);
        });
    }
    // A hook can fill an output pipe without ever reading stdin. The timeout must cover both sides.
    std::thread writer;
    if (stdin_write) {
        writer = std::thread([&] {
            std::string_view input = request.stdin_data;
            DWORD written = 0;
            while (!input.empty() &&
                   WriteFile(stdin_write.get(), input.data(), static_cast<DWORD>(std::min<std::size_t>(input.size(), 65536)),
                             &written, nullptr) && written) input.remove_prefix(written);
            stdin_write.reset();
        });
    }

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
    if (error_reader.joinable()) error_reader.join();
    if (writer.joinable()) writer.join();
    result.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(clock::now() - began);
    return result;
}

} // namespace ode
