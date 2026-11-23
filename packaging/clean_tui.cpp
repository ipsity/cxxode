// Standalone ConPTY probe for the clean Windows guest. This is test media only.
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

using namespace std::chrono_literals;

void check(bool ok, const char* operation) {
    if (!ok) throw std::runtime_error(std::string(operation) + " (" + std::to_string(GetLastError()) + ")");
}

struct Handle {
    HANDLE value = nullptr;
    ~Handle() { close(); }
    void close() {
        if (value) CloseHandle(value);
        value = nullptr;
    }
};

struct PseudoConsole {
    HPCON value = nullptr;
    ~PseudoConsole() { if (value) ClosePseudoConsole(value); }
};

struct Attributes {
    std::vector<unsigned char> bytes;
    LPPROC_THREAD_ATTRIBUTE_LIST list = nullptr;

    Attributes() {
        SIZE_T needed = 0;
        InitializeProcThreadAttributeList(nullptr, 1, 0, &needed);
        bytes.resize(needed);
        auto* storage = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(bytes.data());
        check(InitializeProcThreadAttributeList(storage, 1, 0, &needed), "InitializeProcThreadAttributeList");
        list = storage;
    }
    ~Attributes() { DeleteProcThreadAttributeList(list); }
};

class Terminal {
public:
    Terminal(const wchar_t* application, const wchar_t* workspace, bool startup_only = false,
             const wchar_t* provider_config = nullptr) {
        Handle app_input, app_output;
        check(CreatePipe(&app_input.value, &input_.value, nullptr, 0), "CreatePipe input");
        check(CreatePipe(&output_.value, &app_output.value, nullptr, 0), "CreatePipe output");
        check(SUCCEEDED(CreatePseudoConsole({120, 32}, app_input.value, app_output.value, 0, &console_.value)),
              "CreatePseudoConsole");
        app_input.close();
        app_output.close();
        Attributes attributes;
        check(UpdateProcThreadAttribute(attributes.list, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
              console_.value, sizeof(HPCON), nullptr, nullptr), "UpdateProcThreadAttribute");
        STARTUPINFOEXW startup{};
        startup.StartupInfo.cb = sizeof(startup);
        // Ask Windows to initialize the new console handles even when the probe is piped.
        startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
        startup.lpAttributeList = attributes.list;
        PROCESS_INFORMATION child{};
        auto command = std::wstring(L"\"") + application + L"\" --tui --ascii --mono --workspace \"" + workspace + L"\"";
        if (provider_config) {
            command += std::wstring(L" --session clean-dual --write pager.ps1 --write pager-check.ps1"
                       L" --allow-run --provider-config \"") + provider_config + L"\"";
        } else {
            command += L" --session clean-p4 --model fixture-edit --write config.txt";
            if (!startup_only) command += L" --resume";
        }
        check(CreateProcessW(application, command.data(), nullptr, nullptr, FALSE, EXTENDED_STARTUPINFO_PRESENT,
              nullptr, workspace, &startup.StartupInfo, &child), "CreateProcessW");
        process_.value = child.hProcess;
        CloseHandle(child.hThread);
    }

    ~Terminal() {
        if (process_.value && WaitForSingleObject(process_.value, 0) == WAIT_TIMEOUT) {
            TerminateProcess(process_.value, 1);
            WaitForSingleObject(process_.value, 5000);
        }
    }

    void send(std::string_view text) {
        DWORD written = 0;
        check(WriteFile(input_.value, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) &&
              written == text.size(), "WriteFile input");
    }

    void expect(std::string_view text, std::size_t start = 0) {
        const auto deadline = std::chrono::steady_clock::now() + 12s;
        while (std::chrono::steady_clock::now() < deadline) {
            drain();
            if (screen_.find(text, start) != screen_.npos) return;
            if (WaitForSingleObject(process_.value, 10) == WAIT_OBJECT_0) break;
        }
        const auto offset = screen_.size() > 2048 ? screen_.size() - 2048 : 0;
        throw std::runtime_error("TUI did not show " + std::string(text) + "\n" + screen_.substr(offset));
    }

    void key(std::string_view keys, std::string_view marker) {
        drain();
        const auto start = screen_.size();
        send(keys);
        expect(marker, start);
    }

    void exit() {
        send("\x1b");
        const auto deadline = std::chrono::steady_clock::now() + 5s;
        while (WaitForSingleObject(process_.value, 10) == WAIT_TIMEOUT) {
            drain();
            if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("TUI did not exit");
        }
        DWORD code = 1;
        check(GetExitCodeProcess(process_.value, &code) && code == 0, "TUI failed");
    }

    double startup_ms() const {
        FILETIME created{}, exited{}, kernel{}, user{}, now{};
        GetSystemTimePreciseAsFileTime(&now);
        check(GetProcessTimes(process_.value, &created, &exited, &kernel, &user), "GetProcessTimes");
        const auto ticks = [](FILETIME time) {
            return (static_cast<std::uint64_t>(time.dwHighDateTime) << 32) | time.dwLowDateTime;
        };
        return static_cast<double>(ticks(now) - ticks(created)) / 10000.0;
    }

private:
    void drain() {
        DWORD available = 0;
        char buffer[4096];
        while (PeekNamedPipe(output_.value, nullptr, 0, nullptr, &available, nullptr) && available) {
            DWORD read = 0;
            if (!ReadFile(output_.value, buffer, std::min<DWORD>(available, sizeof(buffer)), &read, nullptr) || !read) {
                break;
            }
            screen_.append(buffer, read);
        }
    }
    // Pipe handles close before ConPTY, so an aborted test cannot block draining output.
    PseudoConsole console_;
    Handle input_, output_, process_;
    std::string screen_;
};

} // namespace

int wmain(int argc, wchar_t** argv) {
    const bool startup_only = argc == 4 && std::wstring_view(argv[3]) == L"--startup";
    const bool two_files = argc == 5 && std::wstring_view(argv[3]) == L"--two-files";
    if (argc != 3 && !startup_only && !two_files) return 2;
    try {
        if (two_files) {
            Terminal terminal(argv[1], argv[2], false, argv[4]);
            terminal.expect("Ready for a task");
            terminal.key("Fix one-based page offsets and add a regression test.", "> Fix one-based");
            terminal.key("\x1bOQ", "Needs approval / Edit pager.ps1");
            terminal.expect("($Number - 1) * $Size");
            const auto approve = [&] {
                terminal.key("\t", "y or yes + Enter approves");
                terminal.key("y", "> y");
                terminal.send("\r");
            };
            approve();
            terminal.expect("Needs approval / Edit pager-check.ps1");
            terminal.expect("(Get-PageStart 2 2) -ne 2");
            approve();
            terminal.expect("Needs approval / Run");
            approve();
            terminal.expect("Complete");
            terminal.expect("[edit progress] pager.ps1 [applied]");
            terminal.expect("[edit progress] pager-check.ps1 [applied]");
            terminal.exit();
            std::puts("TUI_TWO_FILE_VERIFICATION_PASS");
            return 0;
        }
        if (startup_only) {
            std::printf("{\"terminal\":\"Native Windows ConPTY\",\"samples_ms\":[");
            for (int i = 0; i <= 100; ++i) {
                Terminal terminal(argv[1], argv[2], true);
                terminal.expect("Ready for a task");
                terminal.expect("F2 submits draft.");
                std::printf("%s%.3f", i ? "," : "", terminal.startup_ms());
                terminal.exit();
            }
            std::puts("]}");
            return 0;
        }
        Terminal terminal(argv[1], argv[2]);
        terminal.expect("F2 resumes the current task.");
        terminal.key("\x1bOQ", "Needs approval / Edit config.txt");
        terminal.expect("+empty_config = allow");
        terminal.key("\t", "y or yes + Enter approves");
        terminal.key("y", "> y");
        terminal.key("\r", "Complete");
        terminal.key("Verify from TUI", "> Verify from TUI");
        terminal.key("\x1bOQ", "Complete");
        terminal.expect("#2 You  Verify from TUI");
        terminal.exit();
        std::puts("TUI_EDIT_FOLLOW_UP_PASS");
        return 0;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\n", error.what());
        return 1;
    }
}
