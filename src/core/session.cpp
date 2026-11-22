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

bool preflight(Round& round) {
    bool valid = true;
    for (auto& record : round.tools) {
        const auto error = validate(record.call);
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

void settle_tools(Session& session, const ExecuteTool& execute,
                  const StopRequested& stop_requested, std::size_t tool_limit,
                  const AuthorizeTool& authorize) {
    std::string_view skip_reason;
    for (auto& record : session.rounds.back().tools) {
        if (stop_requested()) {
            session.stop = SessionStop::cancelled;
            skip_reason = name(session.stop);
        } else if (skip_reason.empty() && session.dispatches == tool_limit) {
            session.stop = SessionStop::tool_limit;
            skip_reason = name(session.stop);
        }
        if (!skip_reason.empty()) {
            record.result = {ToolStatus::skipped, std::string(skip_reason)};
            continue;
        }
        if (authorize) {
            const auto denial = authorize(record.call);
            if (!denial.empty()) {
                record.result = {ToolStatus::failed, std::string(denial)};
                skip_reason = "prior_tool_failed";
                continue;
            }
        }
        if (stop_requested()) {
            session.stop = SessionStop::cancelled;
            skip_reason = name(session.stop);
            record.result = {ToolStatus::skipped, std::string(skip_reason)};
            continue;
        }
        record.dispatched = true;
        ++session.dispatches;
        record.result = execute(record.call);
        if (record.result.status != ToolStatus::ok) {
            record.result.status = ToolStatus::failed;
            skip_reason = "prior_tool_failed";
        }
    }
}

} // namespace

Session run_session(std::string question, const Generate& generate, const ExecuteTool& execute,
                    const StopRequested& stop_requested, Limits limits, const AuthorizeTool& authorize) {
    Session session;
    session.question = std::move(question);

    while (true) {
        if (stop_requested()) {
            session.stop = SessionStop::cancelled;
            return session;
        }
        if (session.requests >= limits.requests) {
            session.stop = SessionStop::request_limit;
            return session;
        }

        ReplyDraft draft;
        bool interrupted = false;
        const auto result = generate({session.question, session.rounds, limits.requests - session.requests},
                                     [&](const ModelEvent& event) {
            interrupted = interrupted || stop_requested();
            return !interrupted && draft.receive(event);
        });
        session.requests += result.attempts;
        if (interrupted || stop_requested()) {
            session.stop = SessionStop::cancelled;
            return session;
        }
        if (result.failure) {
            session.stop = result.failure->reason;
            session.diagnostic = result.failure->diagnostic;
            return session;
        }
        auto complete = std::move(draft).complete();
        if (!complete) {
            session.stop = SessionStop::protocol_error;
            return session;
        }

        // Publish the whole reply before any tool can observe a call.
        auto& round = session.rounds.emplace_back(std::move(*complete));
        if (round.tools.empty()) {
            return session;
        }
        if (preflight(round)) {
            settle_tools(session, execute, stop_requested, limits.tool_calls, authorize);
        }
        if (session.stop != SessionStop::end_turn) {
            return session;
        }
    }
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
    }
    return "unknown";
}

} // namespace lay
