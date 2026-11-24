#include "provider/models.hpp"
#include "platform/http.hpp"

namespace ode {

std::vector<model_info> list_models(const provider_config& config) {
    std::vector<model_info> models;

    http_request req;
    req.headers = {{"accept", "application/json"}};

    switch (config.kind) {
        case protocol::anthropic:
            // Anthropic doesn't have a public models list API
            // Return known models
            models = {
                {"claude-opus-4-8", "Claude Opus 4.8", "Most capable model", 200000, true, true},
                {"claude-sonnet-4-6", "Claude Sonnet 4.6", "Balanced performance", 200000, true, true},
                {"claude-haiku-4-5-20251001", "Claude Haiku 4.5", "Fast and efficient", 200000, true, true},
                {"claude-3-5-sonnet-20241022", "Claude 3.5 Sonnet", "Previous generation", 200000, true, true},
            };
            break;

        case protocol::openai_chat:
        case protocol::openai_responses: {
            // OpenAI and compatible providers have /v1/models endpoint
            std::string base = config.base_url;
            while (!base.empty() && base.back() == '/') base.pop_back();
            if (!base.ends_with("/v1")) base += "/v1";
            req.url = base + "/models";

            if (!config.api_key.empty()) {
                req.headers.emplace_back("authorization", "Bearer " + config.api_key);
            }

            cancel_token cancel;
            std::string body;
            const auto result = http_send(req, [&](std::string_view chunk) { body += chunk; }, cancel);

            if (result.status == 200) {
                try {
                    auto data = json::parse(body);
                    if (data.contains("data") && data["data"].is_array()) {
                        for (const auto& m : data["data"]) {
                            model_info info;
                            info.id = m.value("id", "");
                            info.name = info.id;

                            // Extract context window if available
                            if (m.contains("context_length")) {
                                info.context_window = m["context_length"];
                            }

                            // Guess capabilities from model name
                            const auto& id = info.id;
                            info.supports_vision = id.find("vision") != std::string::npos ||
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
                if (config.name == "groq") {
                    models = {
                        {"llama-3.3-70b-versatile", "Llama 3.3 70B", "Fast inference", 32768, false, true},
                        {"llama-3.1-70b-versatile", "Llama 3.1 70B", "Versatile model", 32768, false, true},
                        {"mixtral-8x7b-32768", "Mixtral 8x7B", "MoE model", 32768, false, true},
                    };
                } else if (config.name == "openai") {
                    models = {
                        {"gpt-4o", "GPT-4o", "Multimodal flagship", 128000, true, true},
                        {"gpt-4o-mini", "GPT-4o Mini", "Fast and affordable", 128000, true, true},
                        {"gpt-4-turbo", "GPT-4 Turbo", "Previous generation", 128000, true, true},
                        {"gpt-3.5-turbo", "GPT-3.5 Turbo", "Fast and cheap", 16385, false, true},
                    };
                } else if (config.name == "deepseek") {
                    models = {
                        {"deepseek-chat", "DeepSeek Chat", "General chat model", 64000, false, true},
                        {"deepseek-coder", "DeepSeek Coder", "Code-specialized", 64000, false, true},
                    };
                }
            }
            break;
        }
    }

    return models;
}

} // namespace ode
