#include "core/message.hpp"

namespace ode {
namespace {

constexpr const char* kind_names[] = {"text", "tool_use", "tool_result", "thinking", "image", "opaque"};

block::kind kind_from(const std::string& name) {
    for (std::size_t i = 0; i < std::size(kind_names); ++i) {
        if (name == kind_names[i]) return static_cast<block::kind>(i);
    }
    return block::kind::text;
}

} // namespace

block block::make_text(std::string text) {
    block b;
    b.text = std::move(text);
    return b;
}

block block::make_tool_use(std::string id, std::string name, json input) {
    block b;
    b.type = kind::tool_use;
    b.id = std::move(id);
    b.name = std::move(name);
    b.input = std::move(input);
    return b;
}

block block::make_tool_result(std::string id, std::string content, bool is_error) {
    block b;
    b.type = kind::tool_result;
    b.id = std::move(id);
    b.text = std::move(content);
    b.is_error = is_error;
    return b;
}

block block::make_image(std::string media_type, std::string base64_data) {
    block b;
    b.type = kind::image;
    b.media_type = std::move(media_type);
    b.data = std::move(base64_data);
    return b;
}

std::string message::plain_text() const {
    std::string out;
    for (const auto& b : blocks) {
        if (b.type == block::kind::text) out += b.text;
    }
    return out;
}

std::vector<const block*> message::tool_uses() const {
    std::vector<const block*> out;
    for (const auto& b : blocks) {
        if (b.type == block::kind::tool_use) out.push_back(&b);
    }
    return out;
}

json to_json(const message& value) {
    json blocks = json::array();
    for (const auto& b : value.blocks) {
        json item{{"type", kind_names[static_cast<int>(b.type)]}};
        if (!b.text.empty()) item["text"] = b.text;
        if (!b.id.empty()) item["id"] = b.id;
        if (!b.name.empty()) item["name"] = b.name;
        if (!b.input.is_null()) item["input"] = b.input;
        if (!b.signature.empty()) item["signature"] = b.signature;
        if (b.is_error) item["is_error"] = true;
        if (b.type == block::kind::image) {
            item["source"] = {
                {"type", "base64"},
                {"media_type", b.media_type},
                {"data", b.data}
            };
        }
        blocks.push_back(std::move(item));
    }
    return {{"role", value.from == message::role::user ? "user" : "assistant"}, {"content", std::move(blocks)}};
}

message message_from_json(const json& value) {
    message m;
    m.from = value.value("role", "user") == "assistant" ? message::role::assistant : message::role::user;
    for (const auto& item : value.at("content")) {
        block b;
        b.type = kind_from(item.value("type", "text"));
        b.text = item.value("text", "");
        b.id = item.value("id", "");
        b.name = item.value("name", "");
        if (item.contains("input")) b.input = item["input"];
        b.signature = item.value("signature", "");
        b.is_error = item.value("is_error", false);
        if (b.type == block::kind::image && item.contains("source")) {
            const auto& src = item["source"];
            b.media_type = src.value("media_type", "");
            b.data = src.value("data", "");
        }
        m.blocks.push_back(std::move(b));
    }
    return m;
}

double usage::cost_usd(std::string_view model) const {
    // Model pricing (USD per million tokens)
    struct pricing {
        double input;
        double output;
        double cache_read;   // Usually 10% of input
        double cache_write;  // Usually 125% of input
    };

    // Extract base model name (remove window suffix like [1m])
    std::string model_base{model};
    if (auto pos = model_base.find('['); pos != std::string::npos) {
        model_base = model_base.substr(0, pos);
    }

    // Pricing table (as of 2024-2025, USD per million tokens)
    static const std::map<std::string, pricing> prices = {
        // Claude 4.x
        {"claude-opus-4-8",     {15.0,  75.0,  1.50, 18.75}},
        {"claude-opus-4-7",     {15.0,  75.0,  1.50, 18.75}},
        {"claude-opus-4-6",     {15.0,  75.0,  1.50, 18.75}},
        {"claude-sonnet-4-6",   {3.0,   15.0,  0.30,  3.75}},
        {"claude-haiku-4-5-20251001", {0.80, 4.0, 0.08,  1.0}},

        // Claude 3.x (legacy)
        {"claude-3-5-sonnet-20241022", {3.0, 15.0, 0.30, 3.75}},
        {"claude-3-5-sonnet-20240620", {3.0, 15.0, 0.30, 3.75}},
        {"claude-3-opus-20240229",     {15.0, 75.0, 1.50, 18.75}},
        {"claude-3-sonnet-20240229",   {3.0, 15.0, 0.30, 3.75}},
        {"claude-3-haiku-20240307",    {0.25, 1.25, 0.025, 0.3125}},

        // OpenAI GPT-4
        {"gpt-4-turbo", {10.0, 30.0, 0.0, 0.0}},
        {"gpt-4o",      {2.5, 10.0, 0.0, 0.0}},
        {"gpt-4o-mini", {0.15, 0.6, 0.0, 0.0}},
        {"gpt-4",       {30.0, 60.0, 0.0, 0.0}},

        // OpenAI GPT-3.5
        {"gpt-3.5-turbo", {0.5, 1.5, 0.0, 0.0}},

        // Groq (free tier, approximate)
        {"llama-3.3-70b-versatile", {0.0, 0.0, 0.0, 0.0}},
        {"llama-3.1-70b-versatile", {0.0, 0.0, 0.0, 0.0}},

        // DeepSeek
        {"deepseek-chat", {0.14, 0.28, 0.014, 0.175}},
        {"deepseek-coder", {0.14, 0.28, 0.014, 0.175}},

        // Claude Fable 5 (hypothetical, adjust when released)
        {"claude-fable-5", {1.0, 5.0, 0.10, 1.25}},
    };

    // Find pricing
    auto it = prices.find(model_base);
    if (it == prices.end()) {
        // Default to Sonnet pricing if unknown
        it = prices.find("claude-sonnet-4-6");
    }

    const auto& p = it->second;

    // Calculate cost: (tokens / 1,000,000) * price_per_million
    return (input * p.input + output * p.output +
            cache_read * p.cache_read + cache_write * p.cache_write) / 1'000'000.0;
}

} // namespace ode
