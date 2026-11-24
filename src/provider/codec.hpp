#pragma once

#include "provider/provider.hpp"

#include <memory>
#include <string_view>

namespace ode {

// Per-protocol stream state. Receives each SSE event's data and builds the reply.
class stream_parser {
public:
    virtual ~stream_parser() = default;
    virtual void event(std::string_view name, const json& data) = 0;
    // Called when the transport ended; finalises stop reason and completeness.
    virtual model_reply finish() = 0;
};

std::unique_ptr<stream_parser> make_parser(protocol kind, const stream_events& events);

json encode_anthropic(const model_request& request);
json encode_chat(const model_request& request);
json encode_responses(const model_request& request);

std::unique_ptr<stream_parser> make_anthropic_parser(const stream_events& events);
std::unique_ptr<stream_parser> make_chat_parser(const stream_events& events);
std::unique_ptr<stream_parser> make_responses_parser(const stream_events& events);

inline json parse_arguments(const std::string& text) {
    if (text.empty()) return json::object();
    auto value = json::parse(text, nullptr, false);
    return value.is_object() ? value : json();
}

} // namespace ode
