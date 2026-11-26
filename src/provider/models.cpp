#include "provider/models.hpp"
#include "platform/http.hpp"

namespace ode {
namespace {

std::vector<model_info> fallback_models(const provider_config& config) {
    if (config.name == "anthropic" && config.kind == protocol::anthropic) {
        return {
            {"claude-opus-4-8", "Claude Opus 4.8", "Most capable model", 200000, true, true},
            {"claude-sonnet-4-6", "Claude Sonnet 4.6", "Balanced performance", 200000, true, true},
            {"claude-haiku-4-5-20251001", "Claude Haiku 4.5", "Fast and efficient", 200000, true, true},
        };
    }
    if (config.name == "openai") {
        return {
            {"gpt-5", "GPT-5", "Frontier model", 400000, true, true},
            {"gpt-5-mini", "GPT-5 mini", "Faster, lower-cost model", 400000, true, true},
            {"gpt-5-nano", "GPT-5 nano", "Smallest, fastest model", 400000, true, true},
            {"gpt-4.1", "GPT-4.1", "Long-context model", 1047576, true, true},
        };
    }
    if (config.name == "deepseek") {
        return {
            {"deepseek-flash", "DeepSeek Flash", "Fast general model", 1048576, true, true},
            {"deepseek-v4-pro", "DeepSeek V4 Pro", "Flagship model", 1048576, false, true},
        };
    }
    return {};
}

} // namespace

model_list list_models(const provider_config& config) {
    http_request req;
    req.method = "GET";
    req.headers = {{"accept", "application/json"}};
    auto base = config.base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    const bool anthropic = config.kind == protocol::anthropic;
    if (anthropic || !base.ends_with("/v1")) base += "/v1";
    if (!anthropic && base == "https://api.deepseek.com/v1") base.resize(base.size() - 3);
    req.url = base + "/models";

    if (anthropic) req.headers.emplace_back("anthropic-version", "2023-06-01");
    if (!config.api_key.empty()) {
        if (anthropic && !config.bearer) req.headers.emplace_back("x-api-key", config.api_key);
        else req.headers.emplace_back("authorization", "Bearer " + config.api_key);
    }

    cancel_token cancel;
    std::string body;
    const auto result = http_send(req, [&](std::string_view chunk) { body += chunk; }, cancel);
    if (result.status != 200) {
        const auto error = result.status == 0 ? "无法连接模型列表接口" : "模型列表请求失败（HTTP " + std::to_string(result.status) + "）";
        return {fallback_models(config), error};
    }

    std::vector<model_info> models;
    const auto data = json::parse(body, nullptr, false);
    if (const auto entries = data.find("data"); entries != data.end() && entries->is_array()) {
        for (const auto& model : *entries) {
            if (!model.is_object()) continue;
            const auto id = model.find("id");
            if (id == model.end() || !id->is_string() || id->get_ref<const std::string&>().empty()) continue;
            model_info info;
            info.id = id->get_ref<const std::string&>();
            const auto display_name = model.find("display_name");
            info.name = display_name != model.end() && display_name->is_string()
                ? display_name->get_ref<const std::string&>() : info.id;
            info.context_window = known_model_context_window(config, info.id);
            for (const auto* key : {"context_window", "context_length"}) {
                const auto window = model.find(key);
                if (window != model.end() && window->is_number_integer() && *window > 0) {
                    info.context_window = window->get<std::size_t>();
                    break;
                }
            }
            info.supports_vision = info.id.find("claude") != std::string::npos || (!anthropic &&
                (info.id.find("vision") != std::string::npos || info.id.starts_with("gpt-4.1") ||
                 info.id.starts_with("gpt-5") || info.id.find("gpt-4o") != std::string::npos));
            info.supports_tools = true;
            models.push_back(std::move(info));
        }
    }
    if (models.empty()) return {fallback_models(config), "模型列表接口未返回可用模型"};
    return {std::move(models), {}};
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
