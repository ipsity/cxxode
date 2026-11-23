#include "tui_workspace.hpp"
#include "tui_text.hpp"

#include <charconv>
#include <stdexcept>

namespace lay::tui {
namespace {

std::string short_line(std::string_view text) {
    constexpr std::size_t limit = 160;
    const auto line = text.substr(0, text.find('\n'));
    const auto prefix = utf8_prefix(line, limit);
    return std::string(prefix) + (prefix.size() < line.size() ? "..." : "");
}

std::string_view tool_state(const ToolRecord& record) {
    switch (record.result.status) {
        case ToolStatus::pending: return record.dispatched ? "dispatched" : "pending";
        case ToolStatus::ok: return "ok";
        case ToolStatus::failed: return "failed";
        case ToolStatus::skipped: return "skipped";
        case ToolStatus::unknown: return "effect unknown";
    }
    return {};
}

void captured(std::string& detail, std::string_view name, const CapturedOutput& output) {
    if (!output.total_bytes) return;
    detail += "\n" + std::string(name) + " · " + std::to_string(output.total_bytes) + " bytes\n" + output.head;
    if (output.omitted_bytes) {
        detail += "\n[" + std::to_string(output.omitted_bytes) + " bytes omitted]\n";
    }
    detail += output.tail;
}

Transcript project(const Session& snapshot, bool ascii, const std::vector<std::vector<std::string>>& source_notes,
                   std::span<const std::string> edits) {
    Transcript output{{}, snapshot.tasks.size(), task_closed(snapshot), snapshot.stop,
                      safe_text(snapshot.diagnostic, ascii)};
    const auto current_revision = edit_revision(snapshot);
    const auto add = [&](std::string title, std::string_view detail) {
        output.cards.push_back({safe_text(title, ascii), safe_text(detail, ascii, true)});
    };
    std::size_t task = 0;
    const auto tasks_at = [&](std::size_t round) {
        while (task < snapshot.tasks.size() && snapshot.tasks[task].first_round == round) {
            const auto& item = snapshot.tasks[task++];
            add("#" + std::to_string(task) + (ascii ? " You  " : " 你  ") + short_line(item.question), item.question);
        }
    };
    for (std::size_t round = 0; round < snapshot.rounds.size(); ++round) {
        tasks_at(round);
        const auto& item = snapshot.rounds[round];
        for (const auto& tool : item.tools) {
            const auto command = tool.call.name == "run";
            const auto target = text_arg(tool.call, command ? "program" : "path");
            auto title = tool.call.name + "  " + short_line(target) + "  [" + std::string(tool_state(tool)) + "]";
            std::string detail = "task " + std::to_string(task) + " · round " + std::to_string(round + 1) +
                " · call " + tool.call.id;
            if (tool.edit) {
                detail += "\n" + edit_review_details(*tool.edit);
            }
            if (tool.run) {
                detail += "\nexecutable " + quoted_argument(tool.run->program) +
                    "\nworking directory " + quoted_argument(tool.run->directory);
                for (std::size_t i = 0; i < tool.run->arguments.size(); ++i) {
                    detail += "\nargument[" + std::to_string(i) + "] " + quoted_argument(tool.run->arguments[i]);
                }
            }
            if (tool.verification && tool.run) {
                const auto& result = *tool.verification;
                title += "  " + describe(result.process.status, result.process.code);
                detail += "\n" + verification_reference(result, *tool.run, current_revision);
                if (!result.test_summary.empty()) {
                    title += " · " + result.test_summary;
                    detail += "\n" + result.test_summary;
                }
                if (!result.process.diagnostic.empty()) detail += "\n" + result.process.diagnostic;
                if (!result.observed_utc.empty()) detail += "\nobserved " + result.observed_utc;
                detail += "\n";
                detail += verification_scope;
                captured(detail, "stdout", result.process.stdout_text);
                captured(detail, "stderr", result.process.stderr_text);
            } else {
                if (!tool.result.version.empty()) detail += "\nversion " + tool.result.version;
                if (!tool.result.observed_utc.empty()) detail += "\n" + tool.result.observed_utc;
                if (!tool.result.text.empty()) detail += "\n" + tool.result.text;
            }
            add(std::move(title), detail);
        }
        if (!item.text.empty()) {
            auto text = utf8_prefix(item.text, 64 * 1024);
            auto detail = std::string(text);
            if (text.size() < item.text.size()) detail += "\n[full reply in saved session]";
            add((ascii ? "Assistant  " : "助手  ") + short_line(text), detail);
            if (round < source_notes.size()) {
                for (const auto& note : source_notes[round]) add(short_line(note), note);
            }
        }
    }
    tasks_at(snapshot.rounds.size());
    for (const auto& note : edits) add(short_line(note), note);
    return output;
}

} // namespace

std::string safe_text(std::string_view text, bool ascii, bool multiline) {
    std::string output;
    output.reserve(text.size());
    while (!text.empty()) {
        const auto first = static_cast<unsigned char>(text.front());
        const auto length = std::min(scalar_bytes(first), text.size());
        const auto fragment = text.substr(0, length);
        if (!valid_utf8(fragment)) {
            output += '?';
            text.remove_prefix(1);
            continue;
        }
        const auto value = scalar(fragment);
        if (value == '\n' && multiline) {
            output += '\n';
        } else if (value == '\t') {
            output += "    ";
        } else if (value < 0x20 || (value >= 0x7f && value <= 0x9f) ||
                   (value >= 0x202a && value <= 0x202e) || (value >= 0x2066 && value <= 0x2069) ||
                   (ascii && value > 0x7e)) {
            char hex[8];
            const auto end = std::to_chars(hex, hex + sizeof(hex), value, 16).ptr;
            output += "\\u{";
            output.append(hex, end);
            output += '}';
        } else {
            output += fragment;
        }
        text.remove_prefix(length);
    }
    return output;
}

WorkspaceWorker::WorkspaceWorker(const WorkspaceWork& work, bool ascii) : work_(work), ascii_(ascii) {}

WorkspaceWorker::~WorkspaceWorker() {
    stop();
    if (thread_.joinable()) thread_.join();
}

void WorkspaceWorker::launch(WorkspaceCommand command) {
    if (thread_.joinable()) thread_.join();
    {
        const std::lock_guard lock(mutex_);
        view_.busy = true;
        view_.requesting = false;
        view_.stopping = false;
        view_.preview.clear();
        view_.error.clear();
        cancelled_.store(false, std::memory_order_relaxed);
    }
    thread_ = std::jthread([this, command = std::move(command)] { execute(command); });
}

WorkspaceView WorkspaceWorker::view() const {
    const std::lock_guard lock(mutex_);
    return view_;
}

void WorkspaceWorker::stop() {
    {
        const std::lock_guard lock(mutex_);
        cancelled_.store(true, std::memory_order_relaxed);
        view_.stopping = view_.busy;
    }
    ready_.notify_all();
}

void WorkspaceWorker::decide(std::size_t ticket, bool approved) {
    {
        const std::lock_guard lock(mutex_);
        if (!view_.review || view_.review->ticket != ticket || decision_ || view_.stopping) return;
        decision_ = approved;
        view_.review.reset();
    }
    ready_.notify_one();
}

void WorkspaceWorker::publish(const Session& snapshot, std::string_view point,
                              const std::function<std::vector<std::string>(std::size_t)>& citations,
                              std::span<const std::string> edits) {
    if (citations) {
        std::vector<std::vector<std::string>> notes;
        for (std::size_t i = 0; i < snapshot.rounds.size(); ++i) notes.push_back(citations(i));
        source_notes_ = std::move(notes);
    }
    // A request changes the preview and counters only. Keep the committed cards.
    auto transcript = point == "request" ? nullptr : std::make_shared<Transcript>(project(snapshot, ascii_, source_notes_, edits));
    const std::lock_guard lock(mutex_);
    view_.preview.clear();
    view_.requesting = point == "request";
    if (transcript) view_.transcript = std::move(transcript);
}

std::optional<bool> WorkspaceWorker::review(std::size_t task, const ToolCall& call, std::string title,
                                            std::string detail) {
    auto request = std::make_shared<Review>(Review{++tickets_, task, safe_text(call.id, ascii_),
        safe_text(title, ascii_), safe_text(detail, ascii_, true)});
    std::unique_lock lock(mutex_);
    if (cancelled_.load(std::memory_order_relaxed)) return std::nullopt;
    decision_.reset();
    view_.review = std::move(request);
    ready_.wait(lock, [&] { return decision_ || cancelled_.load(std::memory_order_relaxed); });
    view_.review.reset();
    return cancelled_.load(std::memory_order_relaxed) ? std::nullopt : decision_;
}

void WorkspaceWorker::execute(const WorkspaceCommand& command) {
    const WorkspacePort port{
        [&] { return cancelled_.load(std::memory_order_relaxed); },
        [&](const Session& session, std::string_view point, const auto& citations, std::span<const std::string> edits) {
            try { publish(session, point, citations, edits); } catch (...) {}
        },
        [&](std::string_view text) {
            auto scope = safe_text(text, ascii_);
            const std::lock_guard lock(mutex_);
            view_.scope = std::move(scope);
        },
        [&](std::string_view text) {
            auto value = safe_text(short_line(text), ascii_);
            const std::lock_guard lock(mutex_);
            view_.preview = std::move(value);
        },
        [&](std::size_t task, const ToolCall& call, std::string title, std::string details) {
            return review(task, call, std::move(title), std::move(details));
        },
    };
    std::string error;
    try {
        work_(command, port);
    } catch (const std::exception& exception) {
        error = safe_text(exception.what(), ascii_);
    }
    const std::lock_guard lock(mutex_);
    view_.error = std::move(error);
    view_.preview.clear();
    view_.review.reset();
    view_.busy = false;
    view_.requesting = false;
    view_.stopping = false;
}

} // namespace lay::tui
