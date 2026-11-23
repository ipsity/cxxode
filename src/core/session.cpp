#include "core/session.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace lay {
namespace {

class ReplyDraft {
public:
    bool receive(const ModelEvent& event) {
        valid_ = valid_ && !finished_ && std::visit([this](const auto& value) {
            return accept(value);
        }, event);
        return valid_;
    }

    std::optional<Round> complete() && {
        if (!valid_ || !finished_) {
            return std::nullopt;
        }
        return std::move(round_);
    }

private:
    bool accept(const TextDelta& value) {
        if (value.text.size() > 4 * 1024 * 1024 - round_.text.size()) {
            return false;
        }
        round_.text += value.text;
        return true;
    }

    bool accept(const ProposeTool& value) {
        if (value.call.id.empty() || round_.tools.size() == 16 ||
            std::ranges::any_of(round_.tools, [&](const ToolRecord& record) {
                return record.call.id == value.call.id;
            })) {
            return false;
        }
        round_.tools.push_back({value.call, false, {}, round_.text.size()});
        return true;
    }

    bool accept(const ProviderItem& value) {
        if (round_.provider_items.size() >= 32 || value.json.size() > 256 * 1024 - provider_bytes_) {
            return false;
        }
        provider_bytes_ += value.json.size();
        round_.provider_items.push_back({value.json, round_.text.size(), round_.tools.size()});
        return true;
    }
    bool accept(const FinishReply& value) {
        finished_ = value.reason == ReplyStop::tool_use
            ? !round_.tools.empty()
            : value.reason == ReplyStop::end_turn && round_.tools.empty() && !round_.text.empty();
        return finished_;
    }

    Round round_;
    std::size_t provider_bytes_ = 0;
    bool valid_ = true;
    bool finished_ = false;
};

bool preflight(Round& round, bool edit_enabled, bool run_enabled) {
    bool valid = true;
    for (auto& record : round.tools) {
        const auto error = validate(record.call, edit_enabled, run_enabled);
        if (!error.empty()) {
            record.result = {ToolStatus::failed, std::string(error)};
            valid = false;
        }
    }
    if (!valid) {
        for (auto& record : round.tools) {
            if (record.result.status == ToolStatus::pending) {
                record.result = {ToolStatus::skipped, "batch_argument_error"};
            }
        }
    }
    return valid;
}

void save(const SessionJournal& journal, Session& session, std::string_view point) {
    if (journal.save) {
        journal.save(session, point);
    }
}

void record_process(ToolRecord& record, Verification verification) {
    const auto& process = verification.process;
    const auto status = process.status == ProcessStatus::unknown ? ToolStatus::unknown :
        process.status == ProcessStatus::exited && process.code == 0 ? ToolStatus::ok : ToolStatus::failed;
    record.result = {status, verification_text(verification) + "\nObserved at workspace edit revision " +
                             std::to_string(record.run->edit_revision) + "."};
    record.verification = std::move(verification);
}

void settle_tools(Session& session, const ExecuteTool& execute, const StopRequested& stop_requested,
                  std::size_t first_dispatch, std::size_t tool_limit,
                  const AuthorizeTool& authorize, const SessionJournal& journal) {
    std::string_view skip_reason;
    for (auto& record : session.rounds.back().tools) {
        if (record.result.status != ToolStatus::pending) {
            if (record.result.status != ToolStatus::ok) {
                skip_reason = "prior_tool_failed";
            }
            continue;
        }
        if (session.stop == SessionStop::effect_unknown) {
            skip_reason = name(session.stop);
        } else if (stop_requested()) {
            session.stop = SessionStop::cancelled;
            skip_reason = name(session.stop);
        } else if (skip_reason.empty() && session.dispatches - first_dispatch >= tool_limit) {
            session.stop = SessionStop::tool_limit;
            skip_reason = name(session.stop);
        }
        if (!skip_reason.empty()) {
            record.result = {ToolStatus::skipped, std::string(skip_reason)};
            save(journal, session, "result");
            continue;
        }
        EditPreparation edit;
        RunPreparation run;
        if (record.call.name == "edit") {
            edit = journal.prepare_edit(record.call, session);
            record.edit = std::move(edit.evidence);
            if (!edit.rejection.empty()) {
                if (edit.needs_approval) {
                    session.stop = stop_requested() ? SessionStop::cancelled : SessionStop::approval_required;
                    session.diagnostic = std::move(edit.rejection);
                    break;
                }
                record.result = {ToolStatus::failed, std::move(edit.rejection)};
                skip_reason = "prior_tool_failed";
                save(journal, session, "result");
                continue;
            }
        } else if (record.call.name == "run") {
            run = journal.prepare_run(record.call, session);
            record.run = std::move(run.evidence);
            if (run.needs_approval) {
                session.stop = stop_requested() ? SessionStop::cancelled : SessionStop::approval_required;
                session.diagnostic = "interactive command approval required";
                break;
            }
            if (run.rejection) {
                record_process(record, std::move(*run.rejection));
                skip_reason = "prior_tool_failed";
                save(journal, session, "result");
                continue;
            }
        } else if (authorize) {
            const auto denial = authorize(record.call);
            if (!denial.empty()) {
                record.result = {ToolStatus::failed, std::string(denial)};
                skip_reason = "prior_tool_failed";
                save(journal, session, "result");
                continue;
            }
        }
        if (stop_requested()) {
            session.stop = SessionStop::cancelled;
            skip_reason = name(session.stop);
            record.result = {ToolStatus::skipped, std::string(skip_reason)};
            save(journal, session, "result");
            continue;
        }
        record.dispatched = true;
        ++session.dispatches;
        try {
            save(journal, session, "dispatch");
        } catch (...) {
            record.dispatched = false;
            --session.dispatches;
            throw;
        }
        if (run.apply) {
            record_process(record, run.apply());
        } else {
            record.result = edit.apply ? edit.apply() : execute(record.call);
        }
        if (record.result.status == ToolStatus::unknown) {
            session.stop = SessionStop::effect_unknown;
        } else if (record.result.status != ToolStatus::ok) {
            record.result.status = ToolStatus::failed;
            skip_reason = "prior_tool_failed";
        }
        save(journal, session, "result");
        if (record.edit && record.result.status == ToolStatus::ok && journal.cleanup_edit) {
            journal.cleanup_edit(*record.edit);
        }
    }
}

} // namespace

void recover_session(Session& session, const SessionJournal& journal) {
    const auto first = session.tasks.empty() ? 0 : session.tasks.back().first_round;
    if (session.rounds.size() == first || !journal.save) {
        return;
    }
    if (journal.cleanup_edit) {
        for (const auto& round : std::span(session.rounds).subspan(first)) {
            for (const auto& record : round.tools) {
                if (record.edit && record.result.status == ToolStatus::ok) {
                    journal.cleanup_edit(*record.edit);
                }
            }
        }
    }
    bool changed = false;
    for (auto& record : session.rounds.back().tools) {
        if (record.result.status != ToolStatus::pending || !record.dispatched) {
            continue;
        }
        changed = true;
        if (record.run) {
            Verification observed;
            observed.process.status = ProcessStatus::unknown;
            observed.process.diagnostic = "Command was dispatched before interruption. Its effects and cleanup are unknown.";
            record_process(record, std::move(observed));
            session.stop = SessionStop::effect_unknown;
        } else if (record.edit) {
            record.result = journal.recover_edit(*record.edit);
            session.stop = SessionStop::effect_unknown;
        } else if (record.call.name == "edit" || record.call.name == "run") {
            record.result = {ToolStatus::unknown, "A dispatched action is missing its recovery evidence."};
            session.stop = SessionStop::effect_unknown;
        } else {
            // A new observation is safe for reads and counts as a new dispatch.
            record.dispatched = false;
        }
    }
    if (session.stop == SessionStop::effect_unknown) {
        for (auto& record : session.rounds.back().tools) {
            if (record.result.status == ToolStatus::pending) {
                record.result = {ToolStatus::skipped, "effect_unknown"};
                changed = true;
            }
        }
    }
    if (changed) {
        save(journal, session, "recovery");
    }
}

bool task_closed(const Session& session) {
    return session.finished || session.stop == SessionStop::request_limit || session.stop == SessionStop::tool_limit ||
        session.stop == SessionStop::time_limit;
}

TaskActions task_actions(const Session& session) {
    if (session.tasks.empty() || session.stop == SessionStop::storage_error) return {};
    const auto active = std::span(session.rounds).subspan(session.tasks.back().first_round);
    bool pending = false;
    for (const auto& round : active) {
        for (const auto& tool : round.tools) {
            if (tool.result.status != ToolStatus::pending) continue;
            if (tool.dispatched && (tool.call.name == "edit" || tool.call.name == "run")) return {};
            pending = true;
        }
    }
    if (task_closed(session)) return {.follow_up = !pending};
    if (session.stop == SessionStop::effect_unknown) return {.acknowledge_unknown = true};
    return {.resume = true, .close = true};
}

void close_task(Session& session) {
    if (!task_actions(session).close) {
        throw std::invalid_argument("The task cannot be closed here. Review any unknown effects first.");
    }
    for (auto& round : std::span(session.rounds).subspan(session.tasks.back().first_round)) {
        for (auto& tool : round.tools) {
            if (tool.result.status == ToolStatus::pending) {
                tool.result = {ToolStatus::skipped, "task closed by user"};
            }
        }
    }
    if (session.stop == SessionStop::end_turn) session.stop = SessionStop::cancelled;
    session.finished = true;
}

void append_task(Session& session, std::string question, bool allow_edit) {
    if (session.tasks.empty()) {
        throw std::invalid_argument("Follow-ups require a persistent session.");
    }
    const auto first = session.tasks.back().first_round;
    for (const auto& round : std::span(session.rounds).subspan(first)) {
        if (std::ranges::any_of(round.tools, [](const ToolRecord& record) {
            return record.result.status == ToolStatus::pending;
        })) {
            throw std::invalid_argument("The current task has pending tools. Resume it before adding a follow-up.");
        }
    }
    if (!task_actions(session).follow_up) {
        throw std::invalid_argument("The current task is not closed (" + std::string(name(session.stop)) +
                                    "). Resume it before adding a follow-up.");
    }
    session.tasks.back().ended = session.stop;
    session.tasks.push_back({std::move(question), session.rounds.size(), session.requests, session.dispatches, allow_edit});
    session.finished = false;
    session.stop = SessionStop::end_turn;
    session.diagnostic.clear();
}

Session run_session(std::string question, const Generate& generate, const ExecuteTool& execute,
                    const StopRequested& stop_requested, Limits limits, const AuthorizeTool& authorize) {
    Session session;
    session.question = std::move(question);
    return continue_session(std::move(session), generate, execute, stop_requested, limits, authorize, {});
}

Session continue_session(Session session, const Generate& generate, const ExecuteTool& execute,
                         const StopRequested& stop_requested, Limits limits,
                         const AuthorizeTool& authorize, const SessionJournal& journal) {
    const Task* task = session.tasks.empty() ? nullptr : &session.tasks.back();
    const auto first_round = task ? task->first_round : 0;
    const auto first_request = task ? task->first_request : 0;
    const auto first_dispatch = task ? task->first_dispatch : 0;
    const bool edit_enabled = journal.save && journal.prepare_edit && (!task || task->allow_edit);
    const bool run_enabled = journal.save && journal.prepare_run;
    try {
        if (!session.finished) {
            recover_session(session, journal);
        }
        if (session.finished || session.stop == SessionStop::effect_unknown || session.stop == SessionStop::time_limit) {
            return session;
        }
        session.stop = SessionStop::end_turn;
        session.diagnostic.clear();
        while (true) {
            if (session.rounds.size() > first_round) {
                auto& round = session.rounds.back();
                const bool untouched = std::ranges::all_of(round.tools, [](const ToolRecord& record) {
                    return !record.dispatched && record.result.status == ToolStatus::pending;
                });
                if (untouched && !preflight(round, edit_enabled, run_enabled)) {
                    save(journal, session, "result");
                } else {
                    settle_tools(session, execute, stop_requested, first_dispatch, limits.tool_calls, authorize, journal);
                }
            }
            if (session.stop != SessionStop::end_turn || stop_requested() ||
                session.requests - first_request >= limits.requests) {
                if (session.stop == SessionStop::end_turn) {
                    session.stop = stop_requested() ? SessionStop::cancelled : SessionStop::request_limit;
                }
                break;
            }

            ReplyDraft draft;
            bool interrupted = false;
            const auto remaining = limits.requests - (session.requests - first_request);
            if (journal.save) {
                ++session.requests;
                save(journal, session, "request");
                --session.requests;
            }
            const auto result = generate({task ? task->question : session.question, session.rounds, remaining,
                                          edit_enabled, run_enabled, session.tasks},
                                         [&](const ModelEvent& event) {
                interrupted = interrupted || stop_requested();
                return !interrupted && draft.receive(event);
            });
            for (std::size_t i = 0; i < result.observations.size(); ++i) {
                auto attempt = result.observations[i];
                attempt.request_number = session.requests + i + 1;
                session.model_attempts.push_back(std::move(attempt));
            }
            session.requests += result.attempts;
            if (interrupted || stop_requested()) {
                session.stop = SessionStop::cancelled;
                break;
            }
            if (result.failure) {
                session.stop = result.failure->reason;
                session.diagnostic = result.failure->diagnostic;
                break;
            }
            auto complete = std::move(draft).complete();
            if (!complete) {
                session.stop = SessionStop::protocol_error;
                break;
            }

            session.finished = complete->tools.empty();
            session.rounds.emplace_back(std::move(*complete));
            save(journal, session, "reply");
            if (session.finished) {
                return session;
            }
        }
        save(journal, session, "stop");
    } catch (const std::exception& error) {
        if (!journal.save) {
            throw;
        }
        session.finished = false;
        session.stop = SessionStop::storage_error;
        session.diagnostic = error.what();
    }
    return session;
}

std::string edit_review_details(const EditEvidence& evidence) {
    std::string detail = "before " + evidence.before + "\nafter SHA-256 " + evidence.after_digest;
    if (evidence.approved) detail += *evidence.approved ? "\napproval granted" : "\napproval denied";
    if (!evidence.review_diff.empty()) detail += "\n" + evidence.review_diff;
    return detail;
}

std::size_t edit_revision(const Session& session) {
    std::size_t revision = 0;
    for (const auto& round : session.rounds) {
        for (const auto& record : round.tools) {
            if (record.edit && record.dispatched &&
                (record.result.status == ToolStatus::ok || record.result.status == ToolStatus::unknown)) {
                ++revision;
            }
        }
    }
    return revision;
}

std::string_view name(SessionStop stop) {
    switch (stop) {
        case SessionStop::end_turn: return "end_turn";
        case SessionStop::protocol_error: return "protocol_error";
        case SessionStop::cancelled: return "cancelled";
        case SessionStop::request_limit: return "request_limit";
        case SessionStop::tool_limit: return "tool_limit";
        case SessionStop::http_error: return "http_error";
        case SessionStop::transport_error: return "transport_error";
        case SessionStop::response_limit: return "response_limit";
        case SessionStop::refused: return "refused";
        case SessionStop::time_limit: return "time_limit";
        case SessionStop::storage_error: return "storage_error";
        case SessionStop::effect_unknown: return "effect_unknown";
        case SessionStop::approval_required: return "approval_required";
        case SessionStop::context_limit: return "context_limit";
    }
    return "unknown";
}

} // namespace lay
