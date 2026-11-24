#pragma once

#include "core/cancel.hpp"
#include "core/message.hpp"

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace ode {

struct tool_schema {
    std::string name;
    std::string description;
    json parameters;  // JSON Schema object
};

struct model_request {
    std::string model;
    std::string system;
    std::vector<message> messages;
    std::vector<tool_schema> tools;
    std::size_t max_tokens = 16384;
};

enum class stop_reason { end_turn, tool_use, max_tokens, refusal, error, cancelled };

struct model_reply {
    message content{message::role::assistant, {}};
    stop_reason stop = stop_reason::error;
    usage tokens;
    std::string error;  // human readable, set when stop is error
    // A reply only counts as complete when the stream ended with a final event.
    // Tool calls from an incomplete reply are never dispatched.
    bool complete = false;
};

struct stream_events {
    std::function<void(std::string_view)> text;
    std::function<void(std::string_view text, std::string_view signature)> thinking;
    std::function<void(const std::string& name)> tool_started;
};

class provider {
public:
    virtual ~provider() = default;
    virtual model_reply send(const model_request& request, const stream_events& events, cancel_token& cancel) = 0;
};

enum class protocol { anthropic, openai_chat, openai_responses };

struct provider_config {
    std::string name;
    protocol kind = protocol::anthropic;
    std::string base_url;
    std::string api_key;
    bool bearer = false;  // Anthropic: send Authorization: Bearer instead of x-api-key
};

std::unique_ptr<provider> make_provider(const provider_config& config);

// Exposed for offline tests: decode a recorded SSE stream into a reply.
model_reply decode_stream(protocol kind, std::string_view sse_bytes, const stream_events& events = {});
json encode_request(protocol kind, const model_request& request);

} // namespace ode
