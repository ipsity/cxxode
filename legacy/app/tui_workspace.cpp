#include "tui_workspace.hpp"
#include "tui_text.hpp"

#include <charconv>
#include <stdexcept>
#include <utility>

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
        detail += "\n[" + std::to_string(output.omitted_bytes) +
            " bytes omitted during capture. Only the stored head and tail are available.]\n";
    }
    detail += output.tail;
}

Transcript project(const Session& snapshot, bool ascii, const std::vector<std::vector<std::string>>& source_notes,
                   std::span<const std::string> edits, CheckpointView checkpoint, bool settled) {
    Transcript output{{}, snapshot.tasks.size(), task_closed(snapshot), snapshot.stop,
                      safe_text(snapshot.diagnostic, ascii), task_actions(snapshot), checkpoint.revision,
                      !checkpoint.storage_error.empty()};
    output.supplement_saved = !snapshot.tasks.empty() && snapshot.tasks.back().supplement.has_value();
    output.supplement_pending = output.supplement_saved && !snapshot.tasks.back().supplement->before_round && !output.closed;
    const auto current_revision = edit_revision(snapshot);
    const auto add = [&](std::string title, std::string_view detail) {
        output.cards.push_back({safe_text(title, ascii), safe_text(detail, ascii, true)});
    };
    std::size_t task = 0;
    std::string task_files;
    const auto tasks_at = [&](std::size_t round) {
        while (task < snapshot.tasks.size() && snapshot.tasks[task].first_round == round) {
            const auto& item = snapshot.tasks[task++];
            auto detail = item.question;
            if (item.decision_reference) detail += '\n' + decision_reference_receipt(*item.decision_reference);
            add("#" + std::to_string(task) + (ascii ? " You  " : " 你  ") + short_line(item.question), detail);
            output.cards.back().user_task = task;
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
            if (!command && !target.empty()) detail += "\npath " + quoted_argument(target);
            if (task == snapshot.tasks.size() && task && tool.call.name == "edit") {
                if (!task_files.empty()) task_files += "\n\n";
                task_files += detail + "\n" + std::string(tool_state(tool));
                if (tool.edit) task_files += "\nbefore " + tool.edit->before;
                else if (const auto proposed = text_arg(tool.call, "version"); !proposed.empty()) {
                    task_files += "\nproposed version " + std::string(proposed);
                }
                if (!tool.result.version.empty()) task_files += "\nversion " + tool.result.version;
                if (!tool.result.text.empty()) task_files += "\n" + tool.result.text;
            }
            if (tool.edit) {
                detail += "\n" + edit_review_details(*tool.edit);
                if (task == snapshot.tasks.size() && tool.dispatched && tool.result.status == ToolStatus::ok) {
                    output.saved_edit = safe_text(std::string(ascii ? "Edit saved / " : "修改已保存 · ") + tool.edit->path,
                                                  ascii);
                }
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
                if (tool.dispatched && result.process.status == ProcessStatus::exited && result.process.code != 0) {
                    output.latest_failed_check = output.cards.size();
                }
                title += "  " + describe(result.process.status, result.process.code);
                detail += "\n" + verification_reference(result, *tool.run, current_revision);
                if (!result.test_summary.empty()) {
                    title += " · " + result.test_summary;
                    detail += "\n" + result.test_summary;
                }
                if (!result.process.diagnostic.empty()) detail += "\n" + result.process.diagnostic;
                if (!result.observed_utc.empty()) detail += "\nobserved " + result.observed_utc;
                detail += "\nelapsed " + std::to_string(result.process.elapsed_ms) + " ms";
                if (const auto cases = test_case_details(result); !cases.empty()) detail += '\n' + cases;
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
            if (text.size() < item.text.size()) {
                detail += "\n[" + std::to_string(item.text.size() - text.size()) +
                    " bytes omitted from this view. Full saved reply available with --resume --inspect.]";
            }
            add((ascii ? "Assistant  " : "助手  ") + short_line(text), detail);
            if (round < source_notes.size()) {
                for (const auto& note : source_notes[round]) add(short_line(note), note);
            }
        }
    }
    tasks_at(snapshot.rounds.size());
    if (!snapshot.tasks.empty()) {
        if (!edits.empty()) {
            if (!task_files.empty()) task_files += "\n\n";
            task_files += ascii ? "Authorized paths" : "已授权路径";
            for (const auto& note : edits) task_files += '\n' + note;
        }
        const auto title = std::string(ascii ? "Files in task " : "本任务文件改动 · 任务 ") +
            std::to_string(snapshot.tasks.size());
        output.task_files = Card{title,
            task_files.empty() ? std::string(ascii ? "No file edits recorded." : "本任务暂无文件编辑记录。") :
                                 safe_text(task_files, ascii, true)};
    }
    for (const auto& note : edits) add(short_line(note), note);
    for (std::size_t i = 0; i < snapshot.tasks.size(); ++i) {
        if (const auto& input = snapshot.tasks[i].supplement) {
            const bool closed = i + 1 < snapshot.tasks.size() || output.closed;
            const auto receipt = supplement_receipt(*input, i + 1, closed);
            auto title = receipt;
            if (!ascii && !input->before_round) {
                title = std::string(closed ? "补充已保存 · 任务已结清 " : "补充已保存 · 等待处理 ") + supplement_id(i + 1);
            } else if (!ascii) {
                title = "补充已纳入请求 · " + supplement_id(i + 1);
            }
            add(std::move(title), receipt + '\n' + input->text);
        }
    }
    if (settled && !snapshot.tasks.empty()) {
        auto receipt = task_receipt(snapshot, checkpoint.limits);
        add(std::move(receipt.title), receipt.detail);
    }
    if (!checkpoint.observed_reply.empty()) {
        const auto label = ascii ? "Unsaved reply / " : "回复尚未保存 · ";
        add(std::string(label) + short_line(checkpoint.observed_reply), checkpoint.observed_reply);
    }
    if (output.storage_failed) output.diagnostic = safe_text(checkpoint.storage_error, ascii);
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
        view_.can_supplement = false;
        view_.supplement_waiting = false;
        queued_supplement_.reset();
        accepts_supplement_ = command.action == WorkspaceAction::start || command.action == WorkspaceAction::follow_up ||
            command.action == WorkspaceAction::resume;
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

bool WorkspaceWorker::submit_supplement(std::size_t task, std::string text) {
    {
        const std::lock_guard lock(mutex_);
        if (!view_.can_supplement || view_.stopping || !view_.transcript || view_.transcript->tasks != task) return false;
        queued_supplement_ = std::move(text);
        view_.can_supplement = false;
        view_.supplement_waiting = true;
        view_.review.reset();
    }
    ready_.notify_one();
    return true;
}

void WorkspaceWorker::publish(const Session& snapshot, std::string_view point,
                              const std::function<std::vector<std::string>(std::size_t)>& citations,
                              std::span<const std::string> edits, CheckpointView checkpoint) {
    if (citations) {
        std::vector<std::vector<std::string>> notes;
        for (std::size_t i = 0; i < snapshot.rounds.size(); ++i) notes.push_back(citations(i));
        source_notes_ = std::move(notes);
    }
    // A request changes the preview and counters only. Keep the committed cards.
    const bool attempting = point == "request" || point == "request_result";
    auto transcript = attempting ? nullptr :
        std::make_shared<Transcript>(project(snapshot, ascii_, source_notes_, edits, checkpoint, point == "finished"));
    const std::lock_guard lock(mutex_);
    view_.preview.clear();
    view_.requesting = attempting;
    if (transcript) {
        if (transcript->supplement_saved) view_.supplement_waiting = false;
        view_.can_supplement = accepts_supplement_ && point != "finished" && !transcript->closed &&
            !transcript->storage_failed && transcript->stop != SessionStop::effect_unknown &&
            !transcript->supplement_saved && !view_.supplement_waiting && !view_.stopping;
        view_.transcript = std::move(transcript);
    }
}

std::optional<bool> WorkspaceWorker::review(std::size_t task, const ToolCall& call, std::string title,
                                            std::string detail) {
    auto request = std::make_shared<Review>(Review{++tickets_, task, safe_text(call.id, ascii_),
        safe_text(title, ascii_), safe_text(detail, ascii_, true)});
    std::unique_lock lock(mutex_);
    if (queued_supplement_ || cancelled_.load(std::memory_order_relaxed)) return std::nullopt;
    decision_.reset();
    view_.review = std::move(request);
    ready_.wait(lock, [&] { return decision_ || queued_supplement_ || cancelled_.load(std::memory_order_relaxed); });
    view_.review.reset();
    return cancelled_.load(std::memory_order_relaxed) ? std::nullopt : decision_;
}

void WorkspaceWorker::execute(const WorkspaceCommand& command) {
    const WorkspacePort port{
        [&] { return cancelled_.load(std::memory_order_relaxed); },
        [&](const Session& session, std::string_view point, const auto& citations, std::span<const std::string> edits,
            CheckpointView checkpoint) {
            try { publish(session, point, citations, edits, checkpoint); } catch (...) {}
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
        [&](std::size_t task) -> std::optional<std::string> {
            const std::lock_guard lock(mutex_);
            if (!view_.transcript || view_.transcript->tasks != task) return {};
            return std::exchange(queued_supplement_, std::nullopt);
        },
        [&] {
            const std::lock_guard lock(mutex_);
            return queued_supplement_.has_value();
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
    view_.can_supplement = false;
    view_.supplement_waiting = false;
    queued_supplement_.reset();
}

} // namespace lay::tui
