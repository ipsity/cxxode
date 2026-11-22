#include "core/session.hpp"

#include <algorithm>
#include <optional>
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
        if (open_read_ || !count_bytes(value.text.size())) {
            return false;
        }
        round_.text += value.text;
        return true;
    }

    bool accept(const BeginRead& value) {
        if (open_read_ || value.id.empty() || round_.reads.size() == 16 ||
            !count_bytes(value.id.size()) ||
            std::ranges::any_of(round_.reads, [&](const ReadRecord& record) {
                return record.call.id == value.id;
            })) {
            return false;
        }
        open_read_ = ReadCall{value.id, {}};
        return true;
    }

    bool accept(const PathDelta& value) {
        if (!open_read_ || !count_bytes(value.bytes.size())) {
            return false;
        }
        open_read_->path += value.bytes;
        return true;
    }

    bool accept(const EndRead&) {
        if (!open_read_ || open_read_->path.empty() ||
            open_read_->path.find('\0') != std::string::npos) {
            return false;
        }
        round_.reads.push_back({std::move(*open_read_), false, {}});
        open_read_.reset();
        return true;
    }

    bool accept(const FinishReply& value) {
        if (open_read_) {
            return false;
        }
        if (value.reason == ReplyStop::tool_use) {
            finished_ = !round_.reads.empty();
        } else {
            finished_ = value.reason == ReplyStop::end_turn && round_.reads.empty() &&
                        !round_.text.empty();
        }
        return finished_;
    }

    bool count_bytes(std::size_t bytes) {
        if (bytes > remaining_bytes_) {
            return false;
        }
        remaining_bytes_ -= bytes;
        return true;
    }

    Round round_;
    std::optional<ReadCall> open_read_;
    std::size_t remaining_bytes_ = 4 * 1024 * 1024;
    bool valid_ = true;
    bool finished_ = false;
};

void settle_reads(Session& session, const Read& read, const StopRequested& stop_requested,
                  std::size_t tool_limit) {
    std::string_view skip_reason;
    for (auto& record : session.rounds.back().reads) {
        if (stop_requested()) {
            session.stop = SessionStop::cancelled;
            skip_reason = name(session.stop);
        } else if (skip_reason.empty() && session.dispatches == tool_limit) {
            session.stop = SessionStop::tool_limit;
            skip_reason = name(session.stop);
        }
        if (!skip_reason.empty()) {
            record.result = {ReadStatus::skipped, std::string(skip_reason)};
            continue;
        }
        record.dispatched = true;
        ++session.dispatches;
        record.result = read(record.call);
        if (record.result.status != ReadStatus::ok) {
            record.result.status = ReadStatus::failed;
            skip_reason = "prior_read_failed";
        }
    }
}

} // namespace

Session run_session(std::string question, const Generate& generate, const Read& read,
                    const StopRequested& stop_requested, Limits limits) {
    Session session;
    session.question = std::move(question);

    while (true) {
        if (stop_requested()) {
            session.stop = SessionStop::cancelled;
            return session;
        }
        if (session.requests == limits.requests) {
            session.stop = SessionStop::request_limit;
            return session;
        }

        ReplyDraft draft;
        bool interrupted = false;
        ++session.requests;
        generate({session.question, session.rounds}, [&](const ModelEvent& event) {
            interrupted = interrupted || stop_requested();
            return !interrupted && draft.receive(event);
        });
        if (interrupted || stop_requested()) {
            session.stop = SessionStop::cancelled;
            return session;
        }
        auto complete = std::move(draft).complete();
        if (!complete) {
            session.stop = SessionStop::protocol_error;
            return session;
        }

        // Publish the whole reply before any tool can observe a call.
        auto& round = session.rounds.emplace_back(std::move(*complete));
        if (round.reads.empty()) {
            return session;
        }
        settle_reads(session, read, stop_requested, limits.tool_calls);
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
    }
    return "unknown";
}

} // namespace lay
