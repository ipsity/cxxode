#include "core/utf8.hpp"
#include "model/scripted_model.hpp"

#ifdef LAY_WITH_ANTHROPIC
#include "model/anthropic.hpp"
#include "store/session_store.hpp"
#include "tools/edit.hpp"
#include "tools/read_only.hpp"
#include "tools/run.hpp"
#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <unistd.h>
#endif
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdlib>
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
    std::string id;
    std::string writable;
    std::optional<std::string> question;
    bool resume = false;
    bool accept_unknown = false;
    bool allow_run = false;
};

void usage(std::ostream& stream) {
    stream << "Usage: lay-code [--truncated]\n"
           << "       lay-code --workspace PATH --model MODEL [--exclude RELATIVE_PATH ...]\n"
           << "       Use --session ID to keep a workspace session. Enable --write FILE or --allow-run as needed.\n"
           << "       Use --question TEXT to supply the first question. Reopen with --resume.\n"
           << "       After reviewing an unknown effect, use --resume --accept-unknown to continue.\n"
           << "       lay-code --migrate-session ID\n";
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
        if (option == "--resume" && !result.resume) {
            result.resume = true;
            continue;
        }
        if (option == "--accept-unknown" && !result.accept_unknown) {
            result.accept_unknown = true;
            continue;
        }
        if (option == "--allow-run" && !result.allow_run) {
            result.allow_run = true;
            continue;
        }
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
        } else if (option == "--session" && result.id.empty()) {
            result.id = value;
        } else if (option == "--write" && result.writable.empty()) {
            result.writable = value;
        } else if (option == "--question" && !result.question) {
            result.question = value;
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
    if ((result.id.empty() && (!result.writable.empty() || result.allow_run || result.resume)) ||
        (result.accept_unknown && !result.resume) || (result.question && result.resume) ||
        (result.model == "fixture-edit" && result.writable.empty()) ||
        (result.model == "fixture-verify" && (!result.allow_run || result.writable.empty()))) {
        throw std::invalid_argument("Use --session ID for editing, running or resuming. Supply --question only when starting.");
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

std::string question_input(std::string question) {
    if (question.ends_with('\r')) {
        question.pop_back();
    }
    if (question.size() > 32 * 1024) {
        throw std::invalid_argument("Question exceeds 32768 bytes.");
    }
    if (question.empty() || !lay::valid_utf8(question) ||
        question.find_first_of(std::string_view("\0\r\n", 3)) != question.npos) {
        throw std::invalid_argument("Enter one non-empty UTF-8 question.");
    }
    return question;
}

std::string prompt() {
    std::cout << "lay-code> " << std::flush;
    return question_input(read_line());
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

#ifdef LAY_WITH_ANTHROPIC
void show_command(const lay::RunEvidence& run) {
    std::cout << "  executable ";
    terminal_text(lay::quoted_argument(run.program));
    std::cout << '\n';
    for (std::size_t index = 0; index < run.arguments.size(); ++index) {
        std::cout << "  argument[" << index << "] ";
        terminal_text(lay::quoted_argument(run.arguments[index]));
        std::cout << '\n';
    }
    std::cout << "  working directory ";
    terminal_text(lay::quoted_argument(run.directory));
    std::cout << "\n  timeout " << run.timeout_ms << " ms\n";
}

void show_verification(const lay::RunEvidence& run, const lay::Verification& verification,
                       std::size_t current_revision) {
    const auto& process = verification.process;
    std::cout << "[verification] " << lay::describe(process.status, process.code)
              << " at edit revision " << run.edit_revision;
    if (run.edit_revision < current_revision) {
        std::cout << " (earlier workspace revision)";
    }
    std::cout << '\n';
    show_command(run);
    std::cout << "  elapsed " << process.elapsed_ms << " ms";
    if (!verification.observed_utc.empty()) {
        std::cout << "  observed " << verification.observed_utc;
    }
    std::cout << '\n';
    for (const auto& [name, stream] : {std::pair{"stdout", &process.stdout_text},
                                        std::pair{"stderr", &process.stderr_text}}) {
        std::cout << "  " << name << ' ' << stream->total_bytes << " bytes, "
                  << stream->omitted_bytes << " omitted\n";
        const auto& tail = stream->tail.empty() ? stream->head : stream->tail;
        auto start = tail.size() > 512 ? tail.size() - 512 : 0;
        while (start < tail.size() && lay::continuation(static_cast<unsigned char>(tail[start]))) {
            ++start;
        }
        if (start < tail.size()) {
            terminal_text(std::string_view(tail).substr(start));
            std::cout << '\n';
        }
    }
    for (const auto* detail : {&verification.test_summary, &process.diagnostic}) {
        if (!detail->empty()) {
            terminal_text(*detail);
            std::cout << '\n';
        }
    }
    if (process.descendants_terminated) {
        std::cout << "Remaining descendants were terminated.\n";
    }
}
#endif

int render(const lay::Session& session, bool workspace) {
#ifdef LAY_WITH_ANTHROPIC
    const auto revision = lay::edit_revision(session);
#endif
    for (const auto& round : session.rounds) {
        for (const auto& record : round.tools) {
#ifdef LAY_WITH_ANTHROPIC
            if (record.run && record.verification) {
                show_verification(*record.run, *record.verification, revision);
                continue;
            }
#endif
            if (record.call.name == "edit" && record.result.status != lay::ToolStatus::pending) {
                std::cout << "[edit] ";
                terminal_text(record.result.text);
                std::cout << '\n';
                continue;
            }
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
    for (const auto& attempt : session.model_attempts) {
        if (attempt.first_round > 0) {
            std::cout << "[context] " << attempt.first_round << " older rounds projected as host facts. "
                      << attempt.retained_rounds << " complete rounds retained ("
                      << attempt.body_bytes << '/' << attempt.budget_bytes << " bytes)\n";
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

#ifdef LAY_WITH_ANTHROPIC
lay::Generate fixture_edit_model(std::string path) {
    return [path = std::move(path)](const lay::Request& request, const lay::EventSink& emit) -> lay::Generation {
        using namespace lay;
        const auto propose = [&](ToolCall call) {
            const ModelEvent proposal = ProposeTool{std::move(call)};
            if (emit(proposal)) {
                emit(FinishReply{ReplyStop::tool_use});
            }
        };
        if (request.history.empty()) {
            if (request.question == "create fixture") {
                propose({"create-1", "edit", {{"path", path}, {"version", std::string("absent")},
                                             {"new_text", std::string("empty_config = allow\n")}}});
            } else {
                propose({"read-1", "read", {{"path", path}}});
            }
        } else {
            const auto& previous = request.history.back().tools.front();
            if (previous.call.name == "read" && !previous.result.version.empty()) {
                propose({"edit-1", "edit", {{"path", path}, {"version", previous.result.version},
                    {"old_text", std::string("empty_config = reject")},
                    {"new_text", std::string("empty_config = allow")}}});
            } else if (emit(TextDelta{"The host recorded the result above."})) {
                emit(FinishReply{ReplyStop::end_turn});
            }
        }
        return {};
    };
}

lay::Generate fixture_verify_model(std::string path) {
    return [path = std::move(path)](const lay::Request& request, const lay::EventSink& emit) -> lay::Generation {
        using namespace lay;
        const auto propose = [&](ToolCall call) {
            const ModelEvent proposal = ProposeTool{std::move(call)};
            if (emit(proposal)) {
                emit(FinishReply{ReplyStop::tool_use});
            }
        };
        const auto round = request.history.size();
        if (round == 0 || round == 3) {
            const auto program = request.question == "verify with missing program"
                ? "lay-code-fixture-ctest-unavailable" : "ctest";
            propose({"verify-" + std::to_string(round), "run", {
                {"program", std::string(program)}, {"args", TextList{"--output-on-failure", "-R", "^empty_config$"}}}});
            return {};
        }
        const auto repeat = request.question == "verify and edit again";
        if (round == 1 || (round == 4 && repeat)) {
            propose({"read-" + std::to_string(round), "read", {{"path", path}}});
            return {};
        }
        if (round == 2 || (round == 5 && repeat)) {
            const auto& read = request.history.back().tools.front().result;
            if (!read.version.empty()) {
                propose({"edit-" + std::to_string(round), "edit", {{"path", path}, {"version", read.version},
                    {"old_text", std::string(round == 2 ? "empty_config = reject" : "empty_config = allow")},
                    {"new_text", std::string(round == 2 ? "empty_config = allow" : "empty_config = review")}}});
                return {};
            }
        }
        if (emit(TextDelta{"The host's verification records show the command outcomes and their workspace revisions."})) {
            emit(FinishReply{ReplyStop::end_turn});
        }
        return {};
    };
}

bool interactive_input() {
#ifdef LAY_TEST_HOOKS
    if (std::getenv("LAY_TEST_INTERACTIVE")) {
        return true;
    }
#endif
#ifdef _WIN32
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) != 0;
#else
    return ::isatty(STDIN_FILENO) != 0;
#endif
}

lay::StoreCheckpoint test_checkpoint() {
#ifdef LAY_TEST_HOOKS
    return [](std::string_view stage, std::string_view point, const lay::Session& snapshot) {
        if (snapshot.rounds.empty() || snapshot.rounds.back().tools.empty()) {
            return false;
        }
        const auto& call = snapshot.rounds.back().tools.back().call;
        const auto name = std::string(stage) + "_" + call.name + "_" + std::string(point);
        if (const auto* full = std::getenv("LAY_TEST_FULL"); full && name == full) {
            return true;
        }
        if (const auto* pause = std::getenv("LAY_TEST_PAUSE"); pause && name == pause) {
            std::cout << "[checkpoint] " << name << '\n' << std::flush;
            std::string release;
            std::getline(std::cin, release);
        }
        return false;
    };
#else
    return {};
#endif
}

int run_workspace(const Options& settings) {
    using Clock = std::chrono::steady_clock;
    auto deadline = Clock::time_point::max();
    bool cancellation_announced = false;
    const lay::StopRequested cancel = [&] {
        const auto interrupted = stopping();
        if (interrupted && !cancellation_announced) {
            std::cout << "[session] stopping\n" << std::flush;
            cancellation_announced = true;
        }
        return interrupted || Clock::now() >= deadline;
    };
    const bool persistent = !settings.id.empty();
    const bool editing = !settings.writable.empty();
    const bool fixture = settings.model == "fixture-edit" || settings.model == "fixture-verify";
    const auto root = lay::platform::path_from_utf8(settings.workspace);
    const lay::ReadOnlyWorkspace workspace(root, settings.excluded, editing);
    const auto read = [&](const lay::ToolCall& call) { return workspace.execute(call, cancel); };
    const auto authorize = [&](const lay::ToolCall& call) { return workspace.authorize(call); };
    const auto activate = [&] {
        std::signal(SIGINT, on_interrupt);
        deadline = Clock::now() + std::chrono::minutes(15);
    };
    const auto finish = [&](lay::Session session) {
        if (session.stop == lay::SessionStop::cancelled && !stopping()) {
            session.stop = lay::SessionStop::time_limit;
        }
        return render(session, true);
    };
    const auto show_scope = [&] {
        std::cout << (fixture ? "[scope] Local fixture in " : "[scope] Questions and selected file excerpts from ");
        terminal_text(settings.workspace);
        if (!fixture) {
            std::cout << " go to api.anthropic.com using " << settings.model;
        }
        std::cout << ".\n";
    };
    if (!persistent) {
        const auto model = lay::anthropic_model(settings.model, cancel);
        show_scope();
        auto question = settings.question ? question_input(*settings.question) : prompt();
        activate();
        return finish(lay::run_session(std::move(question), model, read, cancel, {}, authorize));
    }

    std::optional<lay::EditWorkspace> editor;
    if (editing) {
        editor.emplace(workspace, settings.writable);
    }
    const lay::RunWorkspace runner(workspace);
    const lay::SessionScope scope{root, workspace.root_identity(), fixture ? "local" : "api.anthropic.com",
                                  settings.model, settings.writable, settings.excluded, settings.allow_run};
    std::optional<std::string> question;
    if (!settings.resume) {
        show_scope();
        question = settings.question ? question_input(*settings.question) : prompt();
    }
    lay::SessionStore store(settings.id, scope, std::move(question), test_checkpoint());
    auto session = store.load();
    if (settings.accept_unknown) {
        if (session.stop != lay::SessionStop::effect_unknown) {
            throw std::invalid_argument("Resume and review the unknown effect before accepting it.");
        }
        session.stop = lay::SessionStop::end_turn;
        store.save(session, "accept_unknown");
    }
    activate();
    const auto review_answer = [&](std::string_view question) {
        std::cout << question << std::flush;
        const auto waiting = Clock::now();
        auto answer = read_line();
        deadline += Clock::now() - waiting;
        if (answer.ends_with('\r')) {
            answer.pop_back();
        }
        return answer == "y" || answer == "yes" ? lay::Approval::approved : lay::Approval::denied;
    };
    const lay::ReviewEdit approve_edit = [&](const lay::EditEvidence& evidence, std::string_view diff) {
        if (!interactive_input()) {
            return lay::Approval::unavailable;
        }
        std::cout << "[edit review] before=" << evidence.before << " after-sha256=" << evidence.after_digest << '\n';
        terminal_text(diff);
        return review_answer("Apply this edit? [y/N] ");
    };
    const lay::ReviewRun approve_run = [&](const lay::RunEvidence& evidence) {
        if (!interactive_input()) {
            return lay::Approval::unavailable;
        }
        std::cout << "[run review] This executable runs with the current account's permissions.\n";
        show_command(evidence);
        if (!fixture) {
            std::cout << "Output excerpts will be sent to api.anthropic.com.\n";
        }
        return review_answer("Run this command? [y/N] ");
    };
    lay::SessionJournal journal{};
    journal.save = [&](const auto& snapshot, auto point) { store.save(snapshot, point); };
    if (editor) {
        journal.prepare_edit = [&](const auto& call, const auto& snapshot) {
            return editor->prepare(call, snapshot, approve_edit, cancel);
        };
        journal.recover_edit = [&](const auto& evidence) { return editor->recover(evidence); };
        journal.cleanup_edit = [&](const auto& evidence) { editor->cleanup(evidence); };
    }
    if (settings.allow_run) {
        journal.prepare_run = [&](const auto& call, const auto& snapshot) {
            return runner.prepare(call, snapshot, approve_run, cancel);
        };
    }
    lay::Generate model;
    if (settings.model == "fixture-verify") {
        model = fixture_verify_model(settings.writable);
    } else if (fixture) {
        model = fixture_edit_model(settings.writable);
    } else {
        // Inspection and recovery of committed evidence must work without model credentials.
        model = [&, provider = lay::Generate{}](const lay::Request& request, const lay::EventSink& emit) mutable {
            if (!provider) {
                try {
                    provider = lay::anthropic_model(settings.model, cancel);
                } catch (const std::exception& error) {
                    return lay::Generation{0, lay::GenerationFailure{lay::SessionStop::transport_error, error.what()}};
                }
            }
            return provider(request, emit);
        };
    }
    return finish(lay::continue_session(std::move(session), model, read, cancel, {}, authorize, journal));
}
#endif

int run(std::span<const std::string> arguments) {
    if (arguments.size() == 1 && arguments.front() == "--help") {
        usage(std::cout);
        return 0;
    }
    try {
#ifdef LAY_WITH_ANTHROPIC
        if (arguments.size() == 2 && arguments[0] == "--migrate-session") {
            const auto backup = lay::SessionStore::migrate(arguments[1]);
            if (backup.empty()) {
                std::cout << "Session format 2 is already current.\n";
            } else {
                std::cout << "Migrated session to format 2. Backup ";
                terminal_text(backup);
                std::cout << '\n';
            }
            return 0;
        }
#endif
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
        return run_workspace(settings);
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
