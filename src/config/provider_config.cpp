#include "config/provider_config.hpp"

namespace ode {

std::optional<protocol> parse_protocol(std::string_view text) {
    if (text == "anthropic" || text == "messages") return protocol::anthropic;
    if (text == "openai-chat" || text == "chat-completions" || text == "chat") return protocol::openai_chat;
    if (text == "openai-responses" || text == "responses" || text == "openai") return protocol::openai_responses;
    return std::nullopt;
}

std::string_view protocol_name(protocol kind) {
    switch (kind) {
        case protocol::anthropic: return "anthropic";
        case protocol::openai_chat: return "openai-chat";
        case protocol::openai_responses: return "openai-responses";
    }
    return {};
}

} // namespace ode
