#include "provider/models.hpp"
#include "platform/http.hpp"

namespace ode {

std::vector<model_info> list_models(const provider_config& config) {
    std::vector<model_info> models;

    http_request req;
    req.headers = {{"accept", "application/json"}};

    switch (config.kind) {
        case protocol::anthropic:
            // Anthropic doesn't have a public models list API, but try it anyway
            // Some proxies (like PassionAPI) implement it
            {
                std::string base = config.base_url;
                while (!base.empty() && base.back() == '/') base.pop_back();
                req.url = base + "/v1/models";
                req.method = "GET";

                if (!config.api_key.empty()) {
                    if (config.bearer) {
                        req.headers.emplace_back("authorization", "Bearer " + config.api_key);
                    } else {
                        req.headers.emplace_back("x-api-key", config.api_key);
                    }
                }

                cancel_token cancel;
                std::string body;
                http_result result;
                try {
                    result = http_send(req, [&](std::string_view chunk) { body += chunk; }, cancel);
                } catch (...) {
                    // Use the official model fallback when the model endpoint is unreachable.
                }

                if (result.status == 200) {
                    try {
                        auto data = json::parse(body);
                        if (data.contains("data") && data["data"].is_array()) {
                            for (const auto& m : data["data"]) {
                                model_info info;
                                info.id = m.value("id", "");
                                info.name = m.value("display_name", info.id);
                                if (m.contains("context_window")) {
                                    info.context_window = m["context_window"];
                                } else if (m.contains("context_length")) {
                                    info.context_window = m["context_length"];
                                } else {
                                    info.context_window = known_model_context_window(config, info.id);
                                }
                                info.supports_vision = info.id.find("claude") != std::string::npos;
                                info.supports_tools = true;
                                if (!info.id.empty()) {
                                    models.push_back(std::move(info));
                                }
                            }
                        }
                    } catch (...) {}
                }

                // Fallback to known Anthropic models if API call failed
                if (models.empty()) {
                    models = {
                        {"claude-opus-4-8", "Claude Opus 4.8", "Most capable model", 200000, true, true},
                        {"claude-sonnet-4-6", "Claude Sonnet 4.6", "Balanced performance", 200000, true, true},
                        {"claude-haiku-4-5-20251001", "Claude Haiku 4.5", "Fast and efficient", 200000, true, true},
                    };
                }
            }
            break;

        case protocol::openai_chat:
        case protocol::openai_responses: {
            // OpenAI and compatible providers have /v1/models endpoint
            std::string base = config.base_url;
            while (!base.empty() && base.back() == '/') base.pop_back();
            if (config.name == "deepseek" && base.ends_with("/v1")) {
                base.resize(base.size() - 3);
            } else if (!base.ends_with("/v1")) {
                base += "/v1";
            }
            req.method = "GET";
            req.url = base + "/models";

            if (!config.api_key.empty()) {
                req.headers.emplace_back("authorization", "Bearer " + config.api_key);
            }

            cancel_token cancel;
            std::string body;
            http_result result;
            try {
                result = http_send(req, [&](std::string_view chunk) { body += chunk; }, cancel);
            } catch (...) {
                // Use provider model fallbacks when model discovery is unreachable.
            }

            if (result.status == 200) {
                try {
                    auto data = json::parse(body);
                    if (data.contains("data") && data["data"].is_array()) {
                        for (const auto& m : data["data"]) {
                            model_info info;
                            info.id = m.value("id", "");
                            info.name = info.id;

                            if (m.contains("context_window")) {
                                info.context_window = m["context_window"];
                            } else if (m.contains("context_length")) {
                                info.context_window = m["context_length"];
                            } else {
                                info.context_window = known_model_context_window(config, info.id);
                            }

                            // Guess capabilities from model name
                            const auto& id = info.id;
                            info.supports_vision = id.find("vision") != std::string::npos ||
                                                   id.starts_with("gpt-4.1") || id.starts_with("gpt-5") ||
                                                   id.find("gpt-4o") != std::string::npos ||
                                                   id.find("claude") != std::string::npos;
                            info.supports_tools = true; // Most modern models support tools

                            if (!info.id.empty()) {
                                models.push_back(std::move(info));
                            }
                        }
                    }
                } catch (...) {
                    // Parse error, return empty list
                }
            }

            // If API call failed or returned no models, provide defaults
            if (models.empty()) {
                if (config.name == "openai") {
                    models = {
                        {"gpt-5", "GPT-5", "Frontier model", 400000, true, true},
                        {"gpt-5-mini", "GPT-5 mini", "Faster, lower-cost model", 400000, true, true},
                        {"gpt-5-nano", "GPT-5 nano", "Smallest, fastest model", 400000, true, true},
                        {"gpt-4.1", "GPT-4.1", "Long-context model", 1047576, true, true},
                    };
                } else if (config.name == "deepseek") {
                    models = {
                        {"deepseek-flash", "DeepSeek Flash", "Fast general model", 1048576, true, true},
                        {"deepseek-v4-pro", "DeepSeek V4 Pro", "Flagship model", 1048576, false, true},
                    };
                }
            }
            break;
        }
    }
    return models;
}

std::size_t known_model_context_window(const provider_config& config, std::string_view model) {
    if (config.name == "deepseek" &&
        (model == "deepseek-flash" || model == "deepseek-v4-pro" ||
         model == "deepseek-v4-flash" || model == "deepseek-v4-flash-vision-exp")) {
        return 1048576;
    }
    if (config.name == "anthropic" &&
        (model == "claude-opus-4-8" || model == "claude-opus-4-7" ||
         model == "claude-sonnet-4-6" || model == "claude-haiku-4-5-20251001")) {
        return 200000;
    }
    if (config.name == "openai") {
        if (model.starts_with("gpt-4.1")) return 1047576;
        if (model.starts_with("gpt-5")) return 400000;
        if (model.starts_with("gpt-4o")) return 128000;
    }
    return 0;
}

} // namespace ode
