#pragma once

#include <optional>
#include <string>

namespace ode {

enum class protocol { anthropic, openai_chat, openai_responses };

// Everything a provider needs to talk to one endpoint. Built by config, consumed by provider.
struct provider_config {
    std::string name;
    protocol kind = protocol::anthropic;
    std::string base_url;
    std::string api_key;
    bool bearer = false;   // Authorization: Bearer instead of x-api-key
    std::string default_model;
};

std::optional<protocol> parse_protocol(std::string_view text);

} // namespace ode
