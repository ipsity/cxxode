#include "core/session.hpp"

#include <algorithm>
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

    bool accept(const FinishReply& value) {
        finished_ = value.reason == ReplyStop::tool_use
            ? !round_.tools.empty()
            : value.reason == ReplyStop::end_turn && round_.tools.empty() && !round_.text.empty();
        return finished_;
    }

    Round round_;
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

void save(const SessionJournal& journal, const Session& session, std::string_view point) {
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
                  std::size_t tool_limit, const AuthorizeTool& authorize, const SessionJournal& journal) {
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
        } else if (skip_reason.empty() && session.dispatches == tool_limit) {
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

void recover(Session& session, const SessionJournal& journal) {
    if (session.rounds.empty() || !journal.save) {
        return;
    }
    if (journal.cleanup_edit) {
        for (const auto& round : session.rounds) {
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

} // namespace

Session run_session(std::string question, const Generate& generate, const ExecuteTool& execute,
                    const StopRequested& stop_requested, Limits limits, const AuthorizeTool& authorize) {
    Session session;
    session.question = std::move(question);
    return continue_session(std::move(session), generate, execute, stop_requested, limits, authorize, {});
}

Session continue_session(Session session, const Generate& generate, const ExecuteTool& execute,
                         const StopRequested& stop_requested, Limits limits,
                         const AuthorizeTool& authorize, const SessionJournal& journal) {
    const bool edit_enabled = journal.save && journal.prepare_edit;
    const bool run_enabled = journal.save && journal.prepare_run;
    try {
        recover(session, journal);
        if (session.finished || session.stop == SessionStop::effect_unknown) {
            return session;
        }
        session.stop = SessionStop::end_turn;
        session.diagnostic.clear();
        while (true) {
            if (!session.rounds.empty()) {
                auto& round = session.rounds.back();
                const bool untouched = std::ranges::all_of(round.tools, [](const ToolRecord& record) {
                    return !record.dispatched && record.result.status == ToolStatus::pending;
                });
                if (untouched && !preflight(round, edit_enabled, run_enabled)) {
                    save(journal, session, "result");
                } else {
                    settle_tools(session, execute, stop_requested, limits.tool_calls, authorize, journal);
                }
            }
            if (session.stop != SessionStop::end_turn || stop_requested() ||
                session.requests >= limits.requests) {
                if (session.stop == SessionStop::end_turn) {
                    session.stop = stop_requested() ? SessionStop::cancelled : SessionStop::request_limit;
                }
                break;
            }

            ReplyDraft draft;
            bool interrupted = false;
            const auto remaining = limits.requests - session.requests;
            if (journal.save) {
                ++session.requests;
                save(journal, session, "request");
                --session.requests;
            }
            const auto result = generate({session.question, session.rounds, remaining, edit_enabled, run_enabled},
                                         [&](const ModelEvent& event) {
                interrupted = interrupted || stop_requested();
                return !interrupted && draft.receive(event);
            });
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
        session.stop = SessionStop::storage_error;
        session.diagnostic = error.what();
    }
    return session;
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
    }
    return "unknown";
}

} // namespace lay
