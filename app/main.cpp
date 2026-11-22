#include "core/utf8.hpp"
#include "model/scripted_model.hpp"

#ifdef LAY_WITH_ANTHROPIC
#include "model/anthropic.hpp"
#include "tools/read_only.hpp"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace {

std::atomic_flag interrupted;

void on_interrupt(int) { interrupted.test_and_set(std::memory_order_relaxed); }
bool stopping() { return interrupted.test(std::memory_order_relaxed); }

struct Options {
    lay::Script script = lay::Script::normal;
    std::string workspace;
    std::string model;
    std::vector<std::string> excluded;
};

void usage(std::ostream& stream) {
    stream << "Usage: lay-code [--truncated]\n"
           << "       lay-code --workspace PATH --model ANTHROPIC_MODEL [--exclude RELATIVE_PATH ...]\n";
}

Options options(std::span<const std::string> arguments) {
    Options result;
    if (arguments.size() == 1 && arguments.front() == "--truncated") {
        result.script = lay::Script::truncated;
        return result;
    }
    if (arguments.empty()) {
        return result;
    }
    for (std::size_t index = 0; index < arguments.size();) {
        const auto& option = arguments[index++];
        if (index == arguments.size() || arguments[index].empty()) {
            throw std::invalid_argument("A workspace option is missing its value.");
        }
        const auto& value = arguments[index++];
        if (option == "--workspace" && result.workspace.empty()) {
            result.workspace = value;
        } else if (option == "--model" && result.model.empty()) {
            result.model = value;
        } else if (option == "--exclude" && result.excluded.size() < 32) {
            result.excluded.push_back(value);
        } else {
            throw std::invalid_argument("Invalid, duplicate or excessive workspace option.");
        }
    }
    if (result.workspace.empty() || result.model.empty() || result.model.size() > 128 ||
        !std::ranges::all_of(result.model, [](unsigned char byte) {
            return (byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
                (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.';
        })) {
        throw std::invalid_argument("Choose a workspace and a valid Anthropic model ID.");
    }
    return result;
}

#if defined(_WIN32) && defined(LAY_WITH_ANTHROPIC)
std::optional<std::string> read_windows_console() {
    const auto input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (!GetConsoleMode(input, &mode)) {
        return std::nullopt;
    }
    std::wstring question;
    while (true) {
        wchar_t buffer[512];
        DWORD read = 0;
        if (!ReadConsoleW(input, buffer, 512, &read, nullptr)) {
            throw std::invalid_argument("Could not read from the console.");
        }
        const std::wstring_view chunk(buffer, read);
        const auto newline = chunk.find(L'\n');
        question += chunk.substr(0, newline);
        if (question.size() > 32 * 1024) {
            throw std::invalid_argument("Question exceeds 32768 bytes.");
        }
        if (newline != chunk.npos || read == 0) {
            const auto utf8 = std::filesystem::path(question).u8string();
            return std::string(utf8.begin(), utf8.end());
        }
    }
}

struct ConsoleOutput {
    UINT previous = GetConsoleOutputCP();
    ConsoleOutput() { if (previous) SetConsoleOutputCP(CP_UTF8); }
    ~ConsoleOutput() { if (previous) SetConsoleOutputCP(previous); }
};
#endif

std::string read_line() {
#if defined(_WIN32) && defined(LAY_WITH_ANTHROPIC)
    if (auto console = read_windows_console()) {
        return std::move(*console);
    }
#endif
    std::string question;
    for (char byte; std::cin.get(byte) && byte != '\n';) {
        if (question.size() >= 32 * 1024) {
            throw std::invalid_argument("Question exceeds 32768 bytes.");
        }
        question += byte;
    }
    return question;
}

std::string prompt() {
    std::cout << "lay-code> " << std::flush;
    auto question = read_line();
    if (question.ends_with('\r')) {
        question.pop_back();
    }
    if (question.size() > 32 * 1024) {
        throw std::invalid_argument("Question exceeds 32768 bytes.");
    }
    if (question.empty() || !lay::valid_utf8(question) || question.find('\0') != question.npos) {
        throw std::invalid_argument("Enter one non-empty UTF-8 question.");
    }
    return question;
}

void terminal_text(std::string_view text) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if (byte == 0xc2 && i + 1 < text.size() &&
            static_cast<unsigned char>(text[i + 1]) >= 0x80 &&
            static_cast<unsigned char>(text[i + 1]) <= 0x9f) {
            ++i;
        } else if (byte == '\n' || byte == '\t' || (byte >= 32 && byte != 127)) {
            std::cout.put(text[i]);
        }
    }
}

int render(const lay::Session& session, bool workspace) {
    for (const auto& round : session.rounds) {
        for (const auto& record : round.tools) {
            if (record.dispatched) {
                if (workspace) {
                    std::cout << '[' << record.call.name << "] ";
                } else {
                    std::cout << "[fixture read] ";
                }
                terminal_text(lay::text_arg(record.call, "path"));
                if (workspace) {
                    std::cout << (record.result.status == lay::ToolStatus::ok ? " (ok)" : " (failed)");
                }
                std::cout << '\n';
            }
        }
        if (!round.text.empty()) {
            if (workspace) {
                std::cout << "[assistant]\n";
                auto text = std::string_view(round.text);
                while (!text.empty()) {
                    const auto newline = text.find('\n');
                    std::cout << "> ";
                    terminal_text(text.substr(0, newline));
                    std::cout << '\n';
                    if (newline == text.npos) {
                        break;
                    }
                    text.remove_prefix(newline + 1);
                }
            } else {
                terminal_text(round.text);
                std::cout << '\n';
            }
        }
    }
    std::cout << "[session] " << lay::name(session.stop) << " requests=" << session.requests
              << " dispatches=" << session.dispatches;
    if (!session.diagnostic.empty()) {
        std::cout << " ";
        terminal_text(session.diagnostic);
    }
    std::cout << '\n';
    if (session.stop == lay::SessionStop::cancelled) {
        return 130;
    }
    return session.stop == lay::SessionStop::end_turn ? 0 : 1;
}

int run(std::span<const std::string> arguments) {
    if (arguments.size() == 1 && arguments.front() == "--help") {
        usage(std::cout);
        return 0;
    }
    try {
        const auto settings = options(arguments);
        if (settings.workspace.empty()) {
            auto question = prompt();
            std::signal(SIGINT, on_interrupt);
            return render(lay::run_session(std::move(question), lay::scripted_model(settings.script),
                [](const lay::ToolCall& call) -> lay::ToolResult {
                    if (lay::text_arg(call, "path") == lay::fixture_path) {
                        return {lay::ToolStatus::ok, "empty_config = allow"};
                    }
                    return {lay::ToolStatus::failed, "fixture path not found"};
                }, stopping), false);
        }
#ifdef LAY_WITH_ANTHROPIC
        using Clock = std::chrono::steady_clock;
        auto deadline = Clock::time_point::max();
        const lay::StopRequested cancel = [&] { return stopping() || Clock::now() >= deadline; };
        const auto model = lay::anthropic_model(settings.model, cancel);
        const lay::ReadOnlyWorkspace workspace(lay::platform::path_from_utf8(settings.workspace), settings.excluded);
        std::cout << "[scope] Questions and selected file excerpts from ";
        terminal_text(settings.workspace);
        std::cout << " go to api.anthropic.com using " << settings.model << ".\n";
        auto question = prompt();
        std::signal(SIGINT, on_interrupt);
        deadline = Clock::now() + std::chrono::minutes(15);
        auto session = lay::run_session(std::move(question), model,
            [&](const lay::ToolCall& call) { return workspace.execute(call, cancel); }, cancel, {},
            [&](const lay::ToolCall& call) { return workspace.authorize(call); });
        if (session.stop == lay::SessionStop::cancelled && !stopping()) {
            session.stop = lay::SessionStop::time_limit;
        }
        return render(session, true);
#else
        throw std::invalid_argument("Build with the readonly-release preset to use a real workspace.");
#endif
    } catch (const std::exception& error) {
        std::cerr << error.what() << '\n';
        return 2;
    }
}

} // namespace

#if defined(_WIN32) && defined(LAY_WITH_ANTHROPIC)
int wmain(int argc, wchar_t** argv) {
    const ConsoleOutput output;
    std::vector<std::string> arguments;
    for (int i = 1; i < argc; ++i) {
        const auto utf8 = std::filesystem::path(argv[i]).u8string();
        arguments.emplace_back(utf8.begin(), utf8.end());
    }
    return run(arguments);
}
#else
int main(int argc, char** argv) {
    return run(std::vector<std::string>(argv + 1, argv + argc));
}
#endif
