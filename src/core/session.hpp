#pragma once

#include <cstddef>
#include <functional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace lay {

enum class ReplyStop { end_turn, tool_use };
enum class SessionStop { end_turn, protocol_error, cancelled, request_limit, tool_limit };
enum class ReadStatus { pending, ok, failed, skipped };

struct TextDelta { std::string text; };
struct BeginRead { std::string id; };
struct PathDelta { std::string bytes; };
struct EndRead {};
struct FinishReply { ReplyStop reason; };
using ModelEvent = std::variant<TextDelta, BeginRead, PathDelta, EndRead, FinishReply>;

struct ReadCall {
    std::string id;
    std::string path;
    bool operator==(const ReadCall&) const = default;
};

struct ReadResult {
    ReadStatus status = ReadStatus::pending;
    std::string text;
    bool operator==(const ReadResult&) const = default;
};

struct ReadRecord {
    ReadCall call;
    bool dispatched = false;
    ReadResult result;
    bool operator==(const ReadRecord&) const = default;
};

struct Round {
    std::string text;
    std::vector<ReadRecord> reads;
    bool operator==(const Round&) const = default;
};

struct Session {
    std::string question;
    std::vector<Round> rounds;
    std::size_t requests = 0;
    std::size_t dispatches = 0;
    SessionStop stop = SessionStop::end_turn;
    bool operator==(const Session&) const = default;
};

struct Request {
    std::string_view question;
    std::span<const Round> history;
};

struct Limits {
    std::size_t requests = 24;
    std::size_t tool_calls = 64;
};

// The model stops emitting when the sink returns false. Returning without a
// valid FinishReply leaves the draft uncommitted.
using EventSink = std::function<bool(const ModelEvent&)>;
using Generate = std::function<void(const Request&, const EventSink&)>;
using Read = std::function<ReadResult(const ReadCall&)>;
using StopRequested = std::function<bool()>;

Session run_session(std::string question, const Generate& generate, const Read& read,
                    const StopRequested& stop_requested, Limits limits = {});
std::string_view name(SessionStop stop);

} // namespace lay
