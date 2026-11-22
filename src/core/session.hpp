#pragma once

#include "core/tools.hpp"

#include <cstddef>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace lay {

enum class ReplyStop { end_turn, tool_use };
enum class SessionStop {
    end_turn, protocol_error, cancelled, request_limit, tool_limit,
    http_error, transport_error, response_limit, refused, time_limit,
};

struct TextDelta { std::string text; };
struct ProposeTool { ToolCall call; };
struct FinishReply { ReplyStop reason; };
using ModelEvent = std::variant<TextDelta, ProposeTool, FinishReply>;

struct ToolRecord {
    ToolCall call;
    bool dispatched = false;
    ToolResult result;
    std::size_t text_offset = 0;
    bool operator==(const ToolRecord&) const = default;
};

struct Round {
    std::string text;
    std::vector<ToolRecord> tools;
    bool operator==(const Round&) const = default;
};

struct Session {
    std::string question;
    std::vector<Round> rounds;
    std::size_t requests = 0;
    std::size_t dispatches = 0;
    SessionStop stop = SessionStop::end_turn;
    std::string diagnostic;
    bool operator==(const Session&) const = default;
};

struct Request {
    std::string_view question;
    std::span<const Round> history;
    std::size_t attempts_left;
};

struct GenerationFailure {
    SessionStop reason;
    std::string diagnostic;
};

struct Generation {
    std::size_t attempts = 1;
    std::optional<GenerationFailure> failure;
};

struct Limits {
    std::size_t requests = 24;
    std::size_t tool_calls = 64;
};

// The model stops emitting when the sink returns false. Returning without a
// valid FinishReply leaves the draft uncommitted.
using EventSink = std::function<bool(const ModelEvent&)>;
using Generate = std::function<Generation(const Request&, const EventSink&)>;
using ExecuteTool = std::function<ToolResult(const ToolCall&)>;
using AuthorizeTool = std::function<std::string_view(const ToolCall&)>;
using StopRequested = std::function<bool()>;

Session run_session(std::string question, const Generate& generate, const ExecuteTool& execute,
                    const StopRequested& stop_requested, Limits limits = {},
                    const AuthorizeTool& authorize = {});
std::string_view name(SessionStop stop);

} // namespace lay
