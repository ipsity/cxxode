#include "core/utf8.hpp"
#include "model/scripted_model.hpp"
#include "tui.hpp"

#ifdef LAY_WITH_ANTHROPIC
#include "model/provider.hpp"
#include "store/session_store.hpp"
#include "tools/citations.hpp"
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
#include <cerrno>
#include <poll.h>
#include <pthread.h>
#include <unistd.h>
#endif
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#ifdef LAY_TEST_HOOKS
#include <filesystem>
#include <fstream>
#endif
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

namespace lay { class ReadOnlyWorkspace; }

namespace {

std::atomic_flag interrupted;

void on_interrupt(int) { interrupted.test_and_set(std::memory_order_relaxed); }
bool stopping() { return interrupted.test(std::memory_order_relaxed); }

#if defined(_WIN32) && defined(LAY_WITH_ANTHROPIC)
BOOL WINAPI on_console_control(DWORD event) {
    if (event != CTRL_C_EVENT && event != CTRL_BREAK_EVENT) {
        return FALSE;
    }
    on_interrupt(SIGINT);
    return TRUE;
}
#endif

struct Options {
    lay::Script script = lay::Script::normal;
    std::string workspace;
    std::string model;
    std::string provider;
    std::string provider_config;
    std::vector<std::string> excluded;
    std::string id;
    std::vector<std::string> writable;
    std::optional<std::string> question;
    std::optional<std::string> follow_up;
#ifdef LAY_TEST_HOOKS
    std::optional<std::string> test_supplement;
#endif
    bool resume = false;
    bool inspect = false;
    bool accept_unknown = false;
    bool close_task = false;
    bool no_write = false;
    bool allow_run = false;
    bool list_models = false;
    bool set_provider_key = false;
    bool mono = false;
    bool ascii = false;
};

void usage(std::ostream& stream) {
    stream << "Usage: lay-code [--truncated]\n"
           << "       lay-code --tui [--demo normal|approval|failure|unknown] [--mono] [--ascii]\n"
           << "       lay-code --tui --workspace PATH --session ID [--resume] [--mono] [--ascii]\n"
           << "       lay-code --workspace PATH [--provider NAME] [--model MODEL] [--exclude RELATIVE_PATH ...]\n"
           << "       lay-code --list-models [--provider NAME] [--provider-config PATH]\n"
           << "       lay-code --set-provider-key [--provider NAME] [--provider-config PATH]\n"
           << "       Use --session ID to keep a workspace session. Use up to two --write FILE options.\n"
           << "       Use --allow-run to propose commands for approval.\n"
           << "       Use --question TEXT to supply the first question. Reopen with --resume.\n"
           << "       Use --resume --inspect to view saved results without contacting the model.\n"
           << "       Use --follow-up TEXT [--no-write] to append to a closed session task.\n"
           << "       Use --resume --close-task to close a paused task offline and keep saved results.\n"
           << "       After reviewing an unknown effect, use --resume --accept-unknown to continue.\n"
           << "       Add --close-task to record that acknowledgement without another model request.\n"
           << "       lay-code --migrate-session ID\n";
}

Options options(std::span<const std::string> arguments, bool tui = false) {
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
        if (tui && option == "--mono" && !result.mono) {
            result.mono = true;
            continue;
        }
        if (tui && option == "--ascii" && !result.ascii) {
            result.ascii = true;
            continue;
        }
        if (option == "--resume" && !result.resume) {
            result.resume = true;
            continue;
        }
        if (!tui && option == "--inspect" && !result.inspect) {
            result.inspect = true;
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
        if (option == "--no-write" && !result.no_write) {
            result.no_write = true;
            continue;
        }
        if (option == "--close-task" && !result.close_task) {
            result.close_task = true;
            continue;
        }
        if (option == "--list-models" && !result.list_models) {
            result.list_models = true;
            continue;
        }
        if (option == "--set-provider-key" && !result.set_provider_key) {
            result.set_provider_key = true;
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
        } else if (option == "--provider" && result.provider.empty()) {
            result.provider = value;
        } else if (option == "--provider-config" && result.provider_config.empty()) {
            result.provider_config = value;
        } else if (option == "--exclude" && result.excluded.size() < 32) {
            result.excluded.push_back(value);
        } else if (option == "--session" && result.id.empty()) {
            result.id = value;
        } else if (option == "--write" && result.writable.size() < 2) {
            result.writable.push_back(value);
        } else if (option == "--question" && !result.question) {
            result.question = value;
        } else if (option == "--follow-up" && !result.follow_up) {
            result.follow_up = value;
#ifdef LAY_TEST_HOOKS
        } else if (option == "--test-supplement" && !result.test_supplement) {
            result.test_supplement = value;
#endif
        } else {
            throw std::invalid_argument("Invalid, duplicate or excessive workspace option.");
        }
    }
#ifdef LAY_TEST_HOOKS
    if (result.test_supplement && (tui || !result.resume || result.inspect || result.accept_unknown || result.close_task ||
        result.list_models || result.set_provider_key || result.id.empty())) {
        throw std::invalid_argument("The supplement fixture requires the line CLI with --resume.");
    }
#endif
    if (result.list_models || result.set_provider_key) {
        if ((result.list_models && result.set_provider_key) || !result.workspace.empty() || !result.model.empty() ||
            !result.excluded.empty() || !result.id.empty() || !result.writable.empty() || result.question ||
            result.resume || result.accept_unknown || result.allow_run || result.follow_up ||
            result.no_write || result.close_task || result.inspect) {
            throw std::invalid_argument("Model listing and key setup accept only provider selection options.");
        }
        return result;
    }
    if (result.workspace.empty()) {
        throw std::invalid_argument("Choose a workspace with --workspace PATH.");
    }
    if ((result.id.empty() && (!result.writable.empty() || result.allow_run || result.resume || result.follow_up)) ||
        (result.follow_up && (result.resume || result.question)) ||
        (result.no_write && !result.follow_up) || (result.close_task && !result.resume) ||
        (result.accept_unknown && !result.resume) || (result.question && result.resume) ||
        (result.inspect && (!result.resume || result.close_task || result.accept_unknown)) ||
        (result.model == "fixture-edit" && result.writable.empty()) ||
        (result.model == "fixture-verify" && (!result.allow_run || result.writable.empty()))) {
        throw std::invalid_argument("Use --session ID for persistent tasks. Choose one of --question, --resume or --follow-up.");
    }
    if (result.writable.size() == 2 && (result.model == "fixture-edit" || result.model == "fixture-verify")) {
        throw std::invalid_argument("The built-in editing fixtures take one --write path.");
    }
    std::ranges::sort(result.writable);
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

void terminal_characters(std::string_view text, auto put) {
    for (std::size_t i = 0; i < text.size(); ++i) {
        const auto byte = static_cast<unsigned char>(text[i]);
        if (byte == 0xc2 && i + 1 < text.size() &&
            static_cast<unsigned char>(text[i + 1]) >= 0x80 &&
            static_cast<unsigned char>(text[i + 1]) <= 0x9f) {
            ++i;
        } else if (byte == '\n' || byte == '\t' || (byte >= 32 && byte != 127)) {
            put(text[i]);
        }
    }
}

void terminal_text(std::string_view text) {
    terminal_characters(text, [](char byte) { std::cout.put(byte); });
}

#ifdef LAY_WITH_ANTHROPIC
class PreviewOutput {
public:
    void offer(std::string_view text) {
        auto lock = std::unique_lock(mutex_, std::try_to_lock);
        if (!lock || !pending_.empty()) {
            return;
        }
        text.remove_prefix(std::min(text.find_first_not_of(" \t\r\n"), text.size()));
        const auto shown = lay::utf8_prefix(text.substr(0, text.find_first_of("\r\n")), 240);
        std::string frame = "[preview] ";
        const auto prefix = frame.size();
        terminal_characters(shown, [&](char byte) { frame += byte; });
        if (frame.size() == prefix) {
            return;
        }
        frame += shown.size() < text.size() ? "…\n" : "\n";
        pending_ = std::move(frame);
        lock.unlock();
        ready_.notify_one();
    }

private:
    void write(std::stop_token stop) {
#ifndef _WIN32
        sigset_t ignored;
        sigemptyset(&ignored);
        sigaddset(&ignored, SIGPIPE);
        if (pthread_sigmask(SIG_BLOCK, &ignored, nullptr) != 0) {
            return;
        }
#endif
        while (true) {
            std::string frame;
            {
                auto lock = std::unique_lock(mutex_);
                if (!ready_.wait(lock, stop, [&] { return !pending_.empty(); })) {
                    return;
                }
                frame.swap(pending_);
            }
            if (std::fwrite(frame.data(), 1, frame.size(), stderr) != frame.size() || std::fflush(stderr) != 0) {
                return;
            }
        }
    }

    std::mutex mutex_;
    std::condition_variable_any ready_;
    std::string pending_;
    std::jthread writer_{[this](std::stop_token stop) { write(stop); }};
};

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
    std::cout << "[verification] " << lay::verification_reference(verification, run, current_revision) << '\n';
    show_command(run);
    std::cout << "  elapsed " << process.elapsed_ms << " ms";
    if (!verification.observed_utc.empty()) {
        std::cout << "  observed " << verification.observed_utc;
    }
    std::cout << '\n';
    std::cout << "  " << lay::verification_scope << '\n';
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

int render(const lay::Session& session, const lay::ReadOnlyWorkspace* source_workspace = nullptr,
           bool show_edit_history = false, lay::Limits limits = {}) {
    const bool workspace = source_workspace != nullptr;
#ifdef LAY_WITH_ANTHROPIC
    const auto revision = lay::edit_revision(session);
#endif
    std::size_t task = 0;
    const auto show_tasks = [&](std::size_t round) {
        while (session.tasks.size() > 1 && task < session.tasks.size() && session.tasks[task].first_round == round) {
            std::cout << "[task " << task + 1 << "] ";
            terminal_text(session.tasks[task].question);
            std::cout << '\n';
            ++task;
        }
    };
    for (std::size_t i = 0; i < session.rounds.size(); ++i) {
        show_tasks(i);
        const auto& round = session.rounds[i];
        for (const auto& record : round.tools) {
#ifdef LAY_WITH_ANTHROPIC
            if (record.run && record.verification) {
                show_verification(*record.run, *record.verification, revision);
                continue;
            }
#endif
            if (record.call.name == "edit") {
                if (record.result.status != lay::ToolStatus::pending) {
                    std::cout << "[edit] ";
                    terminal_text(record.result.text);
                    std::cout << '\n';
                }
                if (show_edit_history && record.edit) {
                    std::cout << "[edit evidence] ";
                    terminal_text(record.edit->path + "\n" + lay::edit_review_details(*record.edit));
                    std::cout << '\n';
                }
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
#ifdef LAY_WITH_ANTHROPIC
        if (source_workspace) {
            for (const auto& note : lay::source_evidence(session, i, *source_workspace)) {
                terminal_text(note);
                std::cout << '\n';
            }
        }
#endif
    }
    show_tasks(session.rounds.size());
    for (std::size_t i = 0; i < session.tasks.size(); ++i) {
        if (const auto& input = session.tasks[i].supplement) {
            terminal_text(lay::supplement_receipt(*input, i + 1, i + 1 < session.tasks.size() || lay::task_closed(session)));
            std::cout << '\n';
            terminal_text(input->text);
            std::cout << '\n';
        }
    }
    for (const auto& attempt : session.model_attempts) {
        if (attempt.first_round > 0) {
            std::cout << "[context] " << attempt.first_round << " older rounds projected as host facts. "
                      << attempt.retained_rounds << " complete rounds retained ("
                      << attempt.body_bytes << '/' << attempt.budget_bytes << " bytes)\n";
        }
    }
    const bool pending_supplement = !session.tasks.empty() && session.tasks.back().supplement &&
        !session.tasks.back().supplement->before_round && !lay::task_closed(session);
    const auto reason = pending_supplement && session.stop == lay::SessionStop::end_turn
        ? "supplement_pending" : lay::name(session.stop);
    std::cout << "[session] " << reason << " requests=" << session.requests
              << " dispatches=" << session.dispatches;
    if (!session.diagnostic.empty()) {
        std::cout << " ";
        terminal_text(session.diagnostic);
    }
    std::cout << '\n';
    if (!session.tasks.empty()) {
        const auto receipt = lay::task_receipt(session, limits);
        terminal_text(receipt.title + '\n' + receipt.detail);
        std::cout << '\n';
    }
    if (session.stop == lay::SessionStop::cancelled) {
        return 130;
    }
    return session.stop == lay::SessionStop::end_turn && !pending_supplement ? 0 : 1;
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
        const auto first = request.tasks.empty() ? 0 : request.tasks.back().first_round;
        if (request.history.size() == first) {
            if (request.question == "create fixture") {
                propose({"create-1", "edit", {{"path", path}, {"version", std::string("absent")},
                                             {"new_text", std::string("empty_config = allow\n")}}});
            } else {
                propose({"read-1", "read", {{"path", path}}});
            }
        } else {
            const auto& previous = request.history.back().tools.front();
            if (request.tasks.size() <= 1 && previous.call.name == "read" && !previous.result.version.empty()) {
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
        if (request.tasks.size() > 1) {
            if (round == request.tasks.back().first_round) {
                for (auto previous = request.history.rbegin(); previous != request.history.rend(); ++previous) {
                    for (const auto& record : previous->tools) {
                        if (record.call.name == "run") {
                            auto call = record.call;
                            call.id = "follow-up-" + std::to_string(request.tasks.size());
                            propose(std::move(call));
                            return {};
                        }
                    }
                }
            }
            if (emit(TextDelta{"The host recorded the follow-up verification above."})) {
                emit(FinishReply{ReplyStop::end_turn});
            }
            return {};
        }
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

std::optional<std::string> read_approval() {
#ifdef _WIN32
    const auto input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(input, &mode)) {
        std::string answer;
        while (!stopping()) {
            const auto ready = WaitForSingleObject(input, 10);
            if (ready == WAIT_TIMEOUT) {
                continue;
            }
            INPUT_RECORD record{};
            DWORD read = 0;
            if (ready != WAIT_OBJECT_0 || !ReadConsoleInputW(input, &record, 1, &read)) {
                return std::nullopt;
            }
            if (!read || record.EventType != KEY_EVENT || !record.Event.KeyEvent.bKeyDown) {
                continue;
            }
            const auto& key = record.Event.KeyEvent;
            const auto ch = key.uChar.UnicodeChar;
            if (ch == L'\r' || ch == L'\n') {
                std::cout << '\n' << std::flush;
                return answer;
            }
            if (ch == 3) {
                on_interrupt(SIGINT);
                break;
            }
            if (ch == 26) {
                break;
            }
            for (WORD repeat = 0; repeat < key.wRepeatCount; ++repeat) {
                if (ch == L'\b' && !answer.empty()) {
                    answer.pop_back();
                    std::cout << "\b \b";
                } else if (ch == L'\t' || ch >= 32) {
                    if (answer.size() >= 32 * 1024) {
                        throw std::invalid_argument("Approval input exceeds 32768 bytes.");
                    }
                    const auto visible = ch < 127 ? static_cast<char>(ch) : '?';
                    answer += visible;
                    std::cout.put(visible);
                }
            }
            std::cout << std::flush;
        }
        std::cout << '\n' << std::flush;
        return std::nullopt;
    }
#else
    if (::isatty(STDIN_FILENO)) {
        pollfd input{STDIN_FILENO, POLLIN, 0};
        while (!stopping()) {
            const auto ready = ::poll(&input, 1, 10);
            if (ready > 0) {
                return stopping() ? std::nullopt : std::optional{read_line()};
            }
            if (ready < 0 && errno != EINTR) {
                return std::nullopt;
            }
        }
        return std::nullopt;
    }
#endif
    return stopping() ? std::nullopt : std::optional{read_line()};
}

lay::StoreCheckpoint test_checkpoint() {
#ifdef LAY_TEST_HOOKS
    return [](std::string_view stage, std::string_view point, const lay::Session& snapshot) {
        const bool has_tool = !snapshot.rounds.empty() && !snapshot.rounds.back().tools.empty();
        const bool attempt = point == "request" || point == "request_result";
        const bool standalone = attempt || point == "follow_up" || point == "supplement" || point == "supplement_included" ||
            point == "supplement_resume";
        if (!standalone && !(point == "reply" && snapshot.finished) && !has_tool) {
            return false;
        }
        const auto name = std::string(stage) + "_" + (standalone || !has_tool ? "" :
            snapshot.rounds.back().tools.back().call.name + "_") + std::string(point) +
            (attempt ? "_" + std::to_string(snapshot.requests) : "");
        if (const auto* full = std::getenv("LAY_TEST_FULL"); full && name == full) {
            return true;
        }
        if (const auto* gate = std::getenv("LAY_TEST_GATE_POINT"); gate && name == gate) {
            if (const auto* path = std::getenv("LAY_TEST_GATE")) {
                const auto ready = lay::platform::path_from_utf8(std::string(path) + ".ready");
                const auto release = lay::platform::path_from_utf8(std::string(path) + ".release");
                std::ofstream(ready).close();
                const auto expires = std::chrono::steady_clock::now() + std::chrono::seconds(15);
                while (!std::filesystem::exists(release) && std::chrono::steady_clock::now() < expires) {
                    std::this_thread::sleep_for(std::chrono::milliseconds(10));
                }
            }
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

void close_cancelled_task(lay::Session& session) {
    const auto active = std::span(session.rounds).subspan(session.tasks.back().first_round);
    for (const auto& round : active) {
        if (std::ranges::any_of(round.tools, [](const auto& tool) {
                return tool.dispatched && tool.result.status == lay::ToolStatus::pending;
            })) {
            return;
        }
    }
    for (auto& round : active) {
        for (auto& tool : round.tools) {
            if (tool.result.status == lay::ToolStatus::pending) {
                tool.result = {lay::ToolStatus::skipped, "cancelled"};
            }
        }
    }
    session.finished = true;
}

int run_workspace(const Options& settings, const lay::tui::WorkspacePort* port = nullptr,
                  lay::tui::WorkspaceAction action = lay::tui::WorkspaceAction::start) {
    using Clock = std::chrono::steady_clock;
    constexpr auto task_time = std::chrono::milliseconds(std::chrono::minutes(15));
    auto deadline = Clock::time_point::max();
    bool cancellation_announced = false;
    const auto interrupted_now = [&] { return port ? port->stopping() : stopping(); };
    const lay::StopRequested cancel = [&] {
        const auto interrupted = interrupted_now();
        if (interrupted && !port && !cancellation_announced) {
            std::cout << "[session] stopping\n" << std::flush;
            cancellation_announced = true;
        }
        return interrupted || Clock::now() >= deadline;
    };
    const bool persistent = !settings.id.empty();
    const bool editing = !settings.writable.empty();
    const bool fixture = settings.model == "fixture-edit" || settings.model == "fixture-verify";
    const auto label = [&](std::string_view zh, std::string_view en) { return settings.ascii ? en : zh; };
    lay::Provider selected;
    if (!fixture) {
        selected = lay::load_provider(settings.provider, settings.model,
                                     lay::platform::path_from_utf8(settings.provider_config));
#ifdef LAY_TEST_HOOKS
        if (const auto* base = std::getenv("LAY_TEST_ANTHROPIC_BASE");
            base && selected.protocol == lay::Protocol::anthropic) {
            selected.base_url = base;
            selected = lay::resolve_provider(std::move(selected));
            if (!selected.loopback) throw std::invalid_argument("The Messages fixture requires a loopback URL.");
            selected.api_key_env.clear();
        }
#endif
        if (selected.model.empty()) {
            throw std::invalid_argument("Choose --model MODEL or set model in the provider profile. Use --list-models to see available IDs.");
        }
    } else {
        selected.model = settings.model;
    }
    const auto root = lay::platform::path_from_utf8(settings.workspace);
    const lay::ReadOnlyWorkspace workspace(root, settings.excluded, editing, persistent);
    std::vector<lay::EditWorkspace> editors;
    editors.reserve(settings.writable.size());
    for (const auto& path : settings.writable) {
        if (std::ranges::any_of(editors, [&](const auto& editor) { return editor.allows(path); })) {
            throw std::invalid_argument("Each --write path must be distinct.");
        }
        editors.emplace_back(workspace, path);
    }
    const auto editor_for = [&](std::string_view path) -> const lay::EditWorkspace* {
        const auto editor = std::ranges::find_if(editors, [&](const auto& item) { return item.allows(path); });
        return editor == editors.end() ? nullptr : &*editor;
    };
    const auto progress = [&](const lay::Session& snapshot) {
        return editors.size() == 2 ? lay::edit_progress(snapshot, settings.writable) : std::vector<std::string>{};
    };
    lay::Limits limits;
#ifdef LAY_TEST_HOOKS
    if (const auto* limit = std::getenv("LAY_TEST_REQUEST_LIMIT"); limit && std::string_view(limit) == "2") {
        limits.requests = 2;
    }
#endif
    const auto publish = [&](const lay::Session& snapshot, std::string_view point, lay::tui::CheckpointView checkpoint) {
        if (!port) return;
        checkpoint.limits = limits;
        const auto edits = point == "request" || point == "request_result" ? std::vector<std::string>{} : progress(snapshot);
        if (point == "loaded" || point == "finished") {
            port->checkpoint(snapshot, point, [&](std::size_t round) {
                return lay::source_evidence(snapshot, round, workspace);
            }, edits, checkpoint);
        } else {
            port->checkpoint(snapshot, point, {}, edits, checkpoint);
        }
    };
    const auto read = [&](const lay::ToolCall& call) { return workspace.execute(call, cancel); };
    const auto authorize = [&](const lay::ToolCall& call) { return workspace.authorize(call); };
    const auto activate = [&](std::size_t used_ms = 0) {
        if (!port) {
#ifdef _WIN32
            if (!SetConsoleCtrlHandler(on_console_control, TRUE)) {
                std::signal(SIGINT, on_interrupt);
            }
#else
            std::signal(SIGINT, on_interrupt);
#endif
        }
        deadline = Clock::now() + task_time - std::chrono::milliseconds(
            std::min<std::size_t>(used_ms, task_time.count()));
    };
    const auto finish = [&](lay::Session session, lay::tui::CheckpointView checkpoint = {}) {
        if (!persistent && session.stop == lay::SessionStop::cancelled && !interrupted_now()) {
            session.stop = lay::SessionStop::time_limit;
        }
        if (port) {
            publish(session, "finished", checkpoint);
            return 0;
        }
        for (const auto& note : progress(session)) {
            terminal_text(note);
            std::cout << '\n';
        }
        return render(session, &workspace, editors.size() == 2, limits);
    };
    if (port) {
        port->scope(fixture ? std::string(label("本地离线夹具", "Local fixture")) :
            selected.model + std::string(label(" → ", " -> ")) + selected.endpoint);
        if (action == lay::tui::WorkspaceAction::prepare) return 0;
    }
    const auto show_scope = [&] {
        if (port) return;
        std::cout << (fixture ? "[scope] Local fixture in " : "[scope] Questions and selected file excerpts from ");
        terminal_text(settings.workspace);
        if (!fixture) {
            std::cout << " go to ";
            terminal_text(selected.endpoint);
            std::cout << " using ";
            terminal_text(selected.model);
        }
        std::cout << ".\n";
    };
    std::optional<PreviewOutput> preview_output;
    if (!fixture && !port) {
        try {
            preview_output.emplace();
        } catch (const std::system_error&) {
            // Streaming previews are optional when the writer cannot start.
        }
    }
    const auto preview = [&](std::string_view text) {
        if (port) {
            port->preview(text);
        } else if (preview_output) {
            preview_output->offer(text);
        }
    };
    if (!persistent) {
        const auto model = lay::provider_model(selected, cancel, preview);
        show_scope();
        auto question = settings.question ? question_input(*settings.question) : prompt();
        activate();
        return finish(lay::run_session(std::move(question), model, read, cancel, {}, authorize));
    }

    const lay::RunWorkspace runner(workspace);
    const lay::SessionScope scope{root, workspace.root_identity(), fixture ? "local" : lay::provider_scope(selected),
                                  selected.model, settings.writable, settings.excluded, settings.allow_run};
    std::optional<std::string> question;
    if (!settings.resume && !settings.follow_up) {
        show_scope();
        question = settings.question ? question_input(*settings.question) : prompt();
    }
    lay::SessionStore store(settings.id, scope, std::move(question), test_checkpoint());
    auto session = store.load();
#ifdef LAY_TEST_HOOKS
    if (settings.test_supplement) {
        store.save_supplement(session, session.tasks.size(), *settings.test_supplement);
        finish(std::move(session), {store.revision()});
        return 0;
    }
#endif
    if (settings.follow_up) {
        lay::append_task(session, question_input(*settings.follow_up), !settings.no_write);
        store.save(session, "follow_up");
        show_scope();
    }
    publish(session, "loaded", {store.revision()});
    if (settings.accept_unknown) {
        if (session.stop != lay::SessionStop::effect_unknown || session.finished) {
            throw std::invalid_argument("Resume and review the unknown effect before accepting it.");
        }
        if (settings.close_task) {
            session.finished = true;
            store.save(session, "closed_unknown");
            finish(std::move(session), {store.revision()});
            return 0;
        }
        session.stop = lay::SessionStop::end_turn;
        store.save(session, "accept_unknown");
    }
    activate(session.tasks.back().active_ms);
    auto approval_wait = Clock::duration(std::chrono::milliseconds(session.tasks.back().approval_wait_ms.value_or(0)));
    const auto account_wait = [&](Clock::time_point started) {
        const auto elapsed = Clock::now() - started;
        approval_wait += elapsed;
        deadline += elapsed;
    };
    const auto review_ui = [&](const lay::ToolCall& call, const lay::Session& snapshot,
                               std::string title, std::string details) {
        const auto waiting = Clock::now();
        const auto answer = port->review(snapshot.tasks.size(), call, std::move(title), std::move(details));
        account_wait(waiting);
        if (!answer || interrupted_now()) return lay::Approval::unavailable;
        return *answer ? lay::Approval::approved : lay::Approval::denied;
    };
    const auto review_answer = [&](std::string_view question) {
        std::cout << question << std::flush;
        const auto waiting = Clock::now();
        auto answer = read_approval();
        account_wait(waiting);
        if (!answer || stopping()) {
            return lay::Approval::unavailable;
        }
        if (answer->ends_with('\r')) {
            answer->pop_back();
        }
        return *answer == "y" || *answer == "yes" ? lay::Approval::approved : lay::Approval::denied;
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
            std::cout << "Output excerpts will be sent to ";
            terminal_text(selected.endpoint);
            std::cout << ".\n";
        }
        return review_answer("Run this command? [y/N] ");
    };
    std::string unsaved_reply;
    lay::SessionJournal journal{};
    journal.persist_model_attempts = !fixture;
    const auto record_time = [&](lay::Session& snapshot) {
        const auto activity = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - deadline);
        snapshot.tasks.back().active_ms = std::clamp(task_time + activity, std::chrono::milliseconds{0}, task_time).count();
        snapshot.tasks.back().approval_wait_ms = std::chrono::duration_cast<std::chrono::milliseconds>(approval_wait).count();
    };
    journal.save = [&](auto& snapshot, auto point) {
        record_time(snapshot);
        if (point == "stop" && snapshot.stop == lay::SessionStop::cancelled) {
            if (!interrupted_now()) {
                snapshot.stop = lay::SessionStop::time_limit;
            } else if (port && !snapshot.tasks.back().supplement) {
                close_cancelled_task(snapshot);
            }
        }
        try {
            store.save(snapshot, point);
        } catch (...) {
            if (point == "reply" && snapshot.finished) unsaved_reply = snapshot.rounds.back().text;
            throw;
        }
        publish(snapshot, point, {store.revision()});
    };
    if (port) {
        journal.accept_supplement = [&](lay::Session& snapshot) {
            if (auto text = port->take_supplement(snapshot.tasks.size())) {
                record_time(snapshot);
                store.save_supplement(snapshot, snapshot.tasks.size(), std::move(*text));
                publish(snapshot, "supplement", {store.revision()});
            }
        };
    }
    if (!editors.empty()) {
        journal.prepare_edit = [&](const auto& call, const auto& snapshot) -> lay::EditPreparation {
            const auto* editor = editor_for(lay::text_arg(call, "path"));
            if (!editor) {
                return {{}, editors.size() == 1 ? "edit path is outside the allowed file" :
                    "edit path is outside the allowed files", {}};
            }
            if (!port) return editor->prepare(call, snapshot, approve_edit, cancel);
            return editor->prepare(call, snapshot, [&](const lay::EditEvidence& evidence, std::string_view diff) {
                auto details = std::string(label("文件版本 ", "File version ")) + evidence.before +
                    std::string(label("\n修改后 SHA-256 ", "\nResult SHA-256 ")) + evidence.after_digest + "\n";
                details += diff;
                return review_ui(call, snapshot, std::string(label("编辑 ", "Edit ")) + evidence.path, std::move(details));
            }, cancel);
        };
        journal.recover_edit = [&](const auto& evidence) -> lay::ToolResult {
            if (const auto* editor = editor_for(evidence.path)) return editor->recover(evidence);
            return {lay::ToolStatus::unknown, "effect unknown for " + evidence.path + ". Edit path is outside the saved scope."};
        };
        journal.cleanup_edit = [&](const auto& evidence) {
            if (const auto* editor = editor_for(evidence.path)) editor->cleanup(evidence);
        };
    }
    if (settings.allow_run) {
        journal.prepare_run = [&](const auto& call, const auto& snapshot) {
            if (!port) return runner.prepare(call, snapshot, approve_run, cancel);
            return runner.prepare(call, snapshot, [&](const lay::RunEvidence& evidence) {
                auto details = std::string(label("程序 ", "Program ")) + lay::quoted_argument(evidence.program) +
                    std::string(label("\n目录 ", "\nDirectory ")) + lay::quoted_argument(evidence.directory) +
                    std::string(label("\n权限 当前账户\n超时 ", "\nPermissions Current account\nTimeout ")) +
                    std::to_string(evidence.timeout_ms) +
                    std::string(label(" ms\n编辑版本 ", " ms\nEdit revision ")) + std::to_string(evidence.edit_revision);
                for (std::size_t i = 0; i < evidence.arguments.size(); ++i) {
                    details += std::string(label("\n参数 ", "\nArgument ")) + std::to_string(i) + " " +
                        lay::quoted_argument(evidence.arguments[i]);
                }
                if (!fixture) details += std::string(label("\n输出摘录将发送至 ", "\nOutput excerpts go to ")) + selected.endpoint;
                return review_ui(call, snapshot, std::string(label("运行 ", "Run ")) +
                    std::string(lay::text_arg(call, "program")), std::move(details));
            }, cancel);
        };
    }
    if (settings.inspect || action == lay::tui::WorkspaceAction::inspect) {
        if (!session.finished) lay::recover_session(session, journal);
        finish(std::move(session), {store.revision()});
        return 0;
    }
    if (settings.close_task) {
        if (!session.finished) lay::recover_session(session, journal);
        lay::close_task(session);
        store.save(session, "closed_task");
        if (!port) std::cout << "[saved revision " << store.revision() << "]\n";
        finish(std::move(session), {store.revision()});
        return 0;
    }
#ifdef LAY_TEST_HOOKS
    if (session.tasks.back().supplement && !lay::task_closed(session)) {
        test_checkpoint()("before", "supplement_resume", session);
    }
#endif
    lay::Generate model;
    if (settings.model == "fixture-verify") {
        model = fixture_verify_model(settings.writable.front());
    } else if (fixture) {
        model = fixture_edit_model(settings.writable.front());
    } else {
        // Inspection and recovery of committed evidence must work without model credentials.
        const lay::StopRequested model_stop = [cancel, port] { return cancel() || (port && port->supplement_requested()); };
        model = [&, model_stop, generate = lay::Generate{}](const lay::Request& request, const lay::EventSink& emit) mutable {
            if (!generate) {
                try {
                    generate = lay::provider_model(selected, model_stop, preview, port ? std::function<void()>{
                        [&] { port->preview({}); }} : std::function<void()>{});
                } catch (const std::exception& error) {
                    return lay::Generation{0, lay::GenerationFailure{lay::SessionStop::transport_error, error.what()}};
                }
            }
            return generate(request, emit);
        };
    }
    auto result = lay::continue_session(std::move(session), model, read, cancel, limits, authorize, journal);
    if (result.stop != lay::SessionStop::storage_error) {
        return finish(std::move(result), {store.revision()});
    }
    std::optional<lay::Session> saved;
    try { saved = store.load(); } catch (const std::exception&) {}
    const lay::tui::CheckpointView checkpoint{saved ? store.revision() : -1, result.diagnostic, unsaved_reply};
    if (port) {
        if (saved) publish(*saved, "finished", checkpoint);
        else port->checkpoint({}, "finished", {}, {}, checkpoint);
        return 0;
    }
    if (saved) {
        std::cout << "[saved revision " << store.revision() << "]\n";
        finish(std::move(*saved), checkpoint);
    }
    if (!unsaved_reply.empty()) {
        std::cout << "[observed reply - not saved]\n";
        terminal_text(unsaved_reply);
        std::cout << '\n';
    }
    std::cout << "[session] storage_error ";
    terminal_text(result.diagnostic);
    std::cout << '\n';
    return 1;
}

int workspace_tui(std::span<const std::string> arguments) {
    const auto settings = options(arguments, true);
    if (settings.workspace.empty() || settings.id.empty() || settings.follow_up || settings.accept_unknown || settings.close_task ||
        settings.list_models || settings.set_provider_key) {
        throw std::invalid_argument("Workspace TUI requires --workspace PATH --session ID. Use F2 to submit and --resume to open a saved session.");
    }
    const lay::tui::WorkspaceScreen screen{settings.id, settings.workspace, settings.question.value_or(""),
                                           settings.resume, settings.mono, settings.ascii, !settings.writable.empty()};
    return lay::tui::run_workspace(screen, [&](const lay::tui::WorkspaceCommand& command, const lay::tui::WorkspacePort& port) {
        auto next = settings;
        next.question.reset();
        next.resume = command.action != lay::tui::WorkspaceAction::start;
        if (!next.resume) {
            next.question = command.question;
        } else if (command.action == lay::tui::WorkspaceAction::follow_up) {
            next.resume = false;
            next.follow_up = command.question;
            next.no_write = !command.allow_edit;
        } else if (command.action == lay::tui::WorkspaceAction::close_task ||
                   command.action == lay::tui::WorkspaceAction::close_unknown) {
            next.accept_unknown = command.action == lay::tui::WorkspaceAction::close_unknown;
            next.close_task = true;
        }
        run_workspace(next, &port, command.action);
    });
}

int provider_command(const Options& settings) {
    const auto selected = lay::load_provider(settings.provider, {},
                                            lay::platform::path_from_utf8(settings.provider_config));
    if (settings.list_models) {
        for (const auto& model : lay::list_models(selected)) {
            terminal_text(model);
            std::cout << '\n';
        }
        return 0;
    }
#ifdef _WIN32
    const auto input = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    if (!GetConsoleMode(input, &mode) || !SetConsoleMode(input, mode & ~ENABLE_ECHO_INPUT)) {
        throw std::invalid_argument("Open this command in a Windows console to enter a hidden API key.");
    }
    struct RestoreConsole {
        HANDLE input;
        DWORD mode;
        ~RestoreConsole() { SetConsoleMode(input, mode); }
    } restore{input, mode};
    std::cout << "API key for " << selected.name << " (input hidden): " << std::flush;
    auto key = read_line();
    struct EraseKey {
        std::string& key;
        ~EraseKey() { SecureZeroMemory(key.data(), key.size()); }
    } erase{key};
    if (key.ends_with('\r')) key.pop_back();
    std::cout << '\n';
    lay::save_provider_key(selected, key);
    std::cout << "Saved the API key in Windows Credential Manager for " << selected.name << ".\n";
    return 0;
#else
    throw std::invalid_argument("Set " + selected.api_key_env + " in your terminal environment.");
#endif
}
#endif

int run(std::span<const std::string> arguments) {
    if (arguments.size() == 1 && arguments.front() == "--help") {
        usage(std::cout);
        return 0;
    }
    try {
        if (!arguments.empty() && arguments.front() == "--tui") {
            const auto remainder = arguments.subspan(1);
            if (std::ranges::find(remainder, "--workspace") != remainder.end()) {
#ifdef LAY_WITH_ANTHROPIC
                return workspace_tui(remainder);
#else
                throw std::invalid_argument("Build with the readonly-release preset to use a real workspace.");
#endif
            }
            return lay::tui::run(remainder);
        }
#ifdef LAY_WITH_ANTHROPIC
        if (arguments.size() == 2 && arguments[0] == "--migrate-session") {
            const auto backup = lay::SessionStore::migrate(arguments[1]);
            if (backup.empty()) {
                std::cout << "Session format " << lay::SessionStore::format_version << " is already current.\n";
            } else {
                std::cout << "Migrated session to format " << lay::SessionStore::format_version << ". Backup ";
                terminal_text(backup);
                std::cout << '\n';
            }
            return 0;
        }
#endif
        const auto settings = options(arguments);
        if (settings.list_models || settings.set_provider_key) {
#ifdef LAY_WITH_ANTHROPIC
            return provider_command(settings);
#else
            throw std::invalid_argument("Build with the readonly-release preset for online providers.");
#endif
        }
        if (settings.workspace.empty()) {
            auto question = prompt();
            std::signal(SIGINT, on_interrupt);
            return render(lay::run_session(std::move(question), lay::scripted_model(settings.script),
                [](const lay::ToolCall& call) -> lay::ToolResult {
                    if (lay::text_arg(call, "path") == lay::fixture_path) {
                        return {lay::ToolStatus::ok, "empty_config = allow"};
                    }
                    return {lay::ToolStatus::failed, "fixture path not found"};
                }, stopping));
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
