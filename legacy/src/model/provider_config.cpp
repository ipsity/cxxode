#include "model/provider.hpp"
#include "core/utf8.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <stdexcept>

namespace lay {
namespace {

using Json = nlohmann::json;

bool letter(unsigned char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

bool digit(unsigned char value) { return value >= '0' && value <= '9'; }

bool profile_name(std::string_view name) {
    return !name.empty() && name.size() <= 48 && std::ranges::all_of(name, [](unsigned char value) {
        return (value >= 'a' && value <= 'z') || digit(value) || value == '-' || value == '_';
    });
}

bool key_name(std::string_view name) {
    return !name.empty() && name.size() <= 128 && (letter(name.front()) || name.front() == '_') &&
        std::ranges::all_of(name, [](unsigned char value) {
            return letter(value) || digit(value) || value == '_';
        });
}

std::filesystem::path default_config() {
#ifdef _WIN32
    if (const auto* appdata = _wgetenv(L"LOCALAPPDATA"); appdata && *appdata) {
        return std::filesystem::path(appdata) / "LayCode" / "providers.json";
    }
#else
    if (const auto* config = std::getenv("XDG_CONFIG_HOME"); config && *config) {
        return std::filesystem::path(config) / "lay-code" / "providers.json";
    }
    if (const auto* home = std::getenv("HOME"); home && *home) {
        return std::filesystem::path(home) / ".config/lay-code/providers.json";
    }
#endif
    return {};
}

Json load_config(const std::filesystem::path& chosen) {
    const auto path = chosen.empty() ? default_config() : chosen;
    if (path.empty()) {
        return Json::object();
    }
    const auto status = std::filesystem::status(path);
    if (chosen.empty() && status.type() == std::filesystem::file_type::not_found) {
        return Json::object();
    }
    if (!std::filesystem::is_regular_file(status)) {
        throw std::invalid_argument("Provider configuration must be a regular JSON file.");
    }
    constexpr std::size_t limit = 64 * 1024;
    std::ifstream input(path, std::ios::binary);
    std::string contents(limit + 1, '\0');
    input.read(contents.data(), static_cast<std::streamsize>(contents.size()));
    contents.resize(static_cast<std::size_t>(input.gcount()));
    if (input.bad() || contents.empty() || contents.size() > limit) {
        throw std::invalid_argument("Could not read provider configuration within 64 KiB.");
    }
    try {
        const auto config = Json::parse(contents);
        if (config.is_object() && (!config.contains("providers") || config["providers"].is_object())) {
            return config;
        }
    } catch (const Json::exception&) {
        // Do not include the rejected JSON or credentials in diagnostics.
    }
    throw std::invalid_argument("Invalid provider configuration JSON.");
}

} // namespace

std::string_view protocol_name(Protocol protocol) {
    switch (protocol) {
        case Protocol::anthropic: return "anthropic-messages";
        case Protocol::chat_completions: return "chat-completions";
        case Protocol::responses: return "responses";
    }
    return "unknown";
}

bool valid_model_id(std::string_view model) {
    return !model.empty() && model.size() <= 256 && valid_utf8(model) &&
        std::ranges::none_of(model, [](unsigned char value) { return value < 32 || value == 127; });
}

Provider load_provider(std::string_view name, std::string_view model, const std::filesystem::path& chosen) {
    const auto config = load_config(chosen);
    Provider provider;
    try {
        provider.name = name.empty() ? config.value("default_provider", std::string("anthropic")) : std::string(name);
        if (!profile_name(provider.name)) {
            throw std::invalid_argument("Use a lowercase provider name of at most 48 characters.");
        }
        if (provider.name == "anthropic") {
            provider.base_url = "https://api.anthropic.com/v1";
            provider.api_key_env = "ANTHROPIC_API_KEY";
        } else if (provider.name == "openai") {
            provider.protocol = Protocol::chat_completions;
            provider.base_url = "https://api.openai.com/v1";
            provider.api_key_env = "OPENAI_API_KEY";
            provider.openai_chat_options = true;
        } else {
            const auto profiles = config.find("providers");
            if (profiles == config.end() || !profiles->contains(provider.name)) {
                throw std::invalid_argument("Unknown provider. Use anthropic, openai or a configured profile.");
            }
            const auto& profile = profiles->at(provider.name);
            const auto protocol = profile.at("protocol").get<std::string>();
            if (protocol == "responses") {
                provider.protocol = Protocol::responses;
            } else if (protocol == "chat-completions") {
                provider.protocol = Protocol::chat_completions;
            } else {
                throw std::invalid_argument("Use responses or chat-completions for a custom provider.");
            }
            provider.base_url = profile.at("base_url").get<std::string>();
            provider.model = profile.value("model", std::string{});
            const auto& key = profile.at("api_key_env");
            if (!key.is_null()) {
                provider.api_key_env = key.get<std::string>();
                if (!key_name(provider.api_key_env)) {
                    throw std::invalid_argument("api_key_env must contain an environment variable name.");
                }
            }
            provider.openai_chat_options = profile.value("openai_chat_options", false);
        }
    } catch (const Json::exception&) {
        throw std::invalid_argument("Provider configuration must define protocol, base_url and api_key_env.");
    }
    if (!model.empty()) {
        provider.model = model;
    }
    if (!provider.model.empty() && !valid_model_id(provider.model)) {
        throw std::invalid_argument("Choose a valid UTF-8 model ID of at most 256 bytes.");
    }
    return resolve_provider(std::move(provider));
}

std::string provider_scope(const Provider& provider) {
    if (provider.protocol == Protocol::anthropic) {
        return "api.anthropic.com";
    }
    return std::string(protocol_name(provider.protocol)) + "|" + provider.endpoint + "|" + provider.api_key_env;
}

} // namespace lay
