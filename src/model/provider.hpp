#pragma once

#include "core/session.hpp"

#include <filesystem>

namespace lay {

enum class Protocol { anthropic, chat_completions, responses };

using TextPreview = std::function<void(std::string_view)>;

struct Provider {
    std::string name;
    Protocol protocol = Protocol::anthropic;
    std::string base_url;
    std::string endpoint;
    std::string model;
    std::string api_key_env;
    bool openai_chat_options = false;
    bool loopback = false;
};

Provider load_provider(std::string_view name, std::string_view model,
                       const std::filesystem::path& config = {});
std::string provider_scope(const Provider& provider);
std::string_view protocol_name(Protocol protocol);
bool valid_model_id(std::string_view model);

// Completes and validates a user-chosen base URL before reading any credential.
Provider resolve_provider(Provider provider);
Generate provider_model(const Provider& provider, StopRequested stop_requested, TextPreview preview = {});
std::vector<std::string> list_models(const Provider& provider);
std::string provider_key(const Provider& provider);
void save_provider_key(const Provider& provider, std::string_view key);

namespace messages {
Generate loopback_model(std::string model, std::uint16_t port, StopRequested stop_requested);
}

} // namespace lay
