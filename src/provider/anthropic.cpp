#include "provider/codec.hpp"

namespace ode {
namespace {

json encode_block(const block& b) {
    switch (b.type) {
        case block::kind::text: return {{"type", "text"}, {"text", b.text}};
        case block::kind::tool_use: return {{"type", "tool_use"}, {"id", b.id}, {"name", b.name}, {"input", b.input}};
        case block::kind::tool_result:
            return {{"type", "tool_result"}, {"tool_use_id", b.id}, {"content", b.text}, {"is_error", b.is_error}};
        case block::kind::thinking:
            return {{"type", "thinking"}, {"thinking", b.text}, {"signature", b.signature}};
        case block::kind::opaque: return b.input;
    }
    return {};
}

bool belongs(const block& b) {
    if (b.type == block::kind::opaque) return b.name == "anthropic";
    if (b.type == block::kind::thinking) return !b.signature.empty();
    if (b.type == block::kind::text) return !b.text.empty();
    return true;
}

class parser final : public stream_parser {
public:
    explicit parser(const stream_events& events) : events_(events) {}

    void event(std::string_view, const json& data) override {
        const auto type = data.value("type", "");
        if (type == "message_start") {
            read_usage(data["message"].value("usage", json::object()));
        } else if (type == "content_block_start") {
            const auto& cb = data["content_block"];
            const auto kind = cb.value("type", "");
            block b;
            if (kind == "tool_use") {
                b = block::make_tool_use(cb.value("id", ""), cb.value("name", ""), json());
                if (events_.tool_started) events_.tool_started(b.name);
            } else if (kind == "thinking") {
                b.type = block::kind::thinking;
            } else if (kind == "text") {
                b.text = cb.value("text", "");
            } else {
                b.type = block::kind::opaque;
                b.name = "anthropic";
                b.input = cb;
            }
            reply_.content.blocks.push_back(std::move(b));
            arguments_.clear();
        } else if (type == "content_block_delta" && !reply_.content.blocks.empty()) {
            auto& b = reply_.content.blocks.back();
            const auto& delta = data["delta"];
            const auto kind = delta.value("type", "");
            if (kind == "text_delta") {
                const auto text = delta.value("text", "");
                b.text += text;
                if (events_.text) events_.text(text);
            } else if (kind == "input_json_delta") {
                arguments_ += delta.value("partial_json", "");
            } else if (kind == "thinking_delta") {
                const auto text = delta.value("thinking", "");
                b.text += text;
                if (events_.thinking) {
                    std::string sig = data.value("signature", "");
                    events_.thinking(text, sig);
                }
            } else if (kind == "signature_delta") {
                b.signature += delta.value("signature", "");
            }
        } else if (type == "content_block_stop" && !reply_.content.blocks.empty()) {
            auto& b = reply_.content.blocks.back();
            if (b.type == block::kind::tool_use) {
                b.input = parse_arguments(arguments_);
                if (b.input.is_null()) broken_ = true;
            }
        } else if (type == "message_delta") {
            stop_ = data["delta"].value("stop_reason", stop_);
            read_usage(data.value("usage", json::object()));
        } else if (type == "message_stop") {
            stopped_ = true;
        } else if (type == "error") {
            reply_.error = data["error"].value("message", data.dump());
        }
    }

    model_reply finish() override {
        reply_.complete = stopped_ && !broken_ && reply_.error.empty();
        if (!reply_.complete) {
            reply_.stop = stop_reason::error;
            if (reply_.error.empty()) reply_.error = broken_ ? "Tool arguments were not valid JSON." : "The stream ended early.";
        } else if (stop_ == "tool_use") {
            reply_.stop = stop_reason::tool_use;
        } else if (stop_ == "max_tokens") {
            reply_.stop = stop_reason::max_tokens;
        } else if (stop_ == "refusal") {
            reply_.stop = stop_reason::refusal;
        } else {
            reply_.stop = stop_reason::end_turn;
        }
        return std::move(reply_);
    }

private:
    void read_usage(const json& u) {
        if (u.contains("input_tokens")) reply_.tokens.input = u.value("input_tokens", std::size_t{});
        if (u.contains("output_tokens")) reply_.tokens.output = u.value("output_tokens", std::size_t{});
        if (u.contains("cache_read_input_tokens") && u["cache_read_input_tokens"].is_number())
            reply_.tokens.cache_read = u["cache_read_input_tokens"].get<std::size_t>();
        if (u.contains("cache_creation_input_tokens") && u["cache_creation_input_tokens"].is_number())
            reply_.tokens.cache_write = u["cache_creation_input_tokens"].get<std::size_t>();
    }

    const stream_events& events_;
    model_reply reply_;
    std::string arguments_;
    std::string stop_;
    bool stopped_ = false;
    bool broken_ = false;
};

} // namespace

json encode_anthropic(const model_request& request) {
    json messages = json::array();
    for (const auto& m : request.messages) {
        json content = json::array();
        for (const auto& b : m.blocks) {
            if (belongs(b)) content.push_back(encode_block(b));
        }
        if (content.empty()) continue;
        messages.push_back({{"role", m.from == message::role::user ? "user" : "assistant"}, {"content", content}});
    }
    // Cache the stable prefix: tools, system and the conversation up to the latest turn.
    const json ephemeral{{"type", "ephemeral"}};
    if (!messages.empty()) messages.back()["content"].back()["cache_control"] = ephemeral;
    json tools = json::array();
    for (const auto& t : request.tools) {
        tools.push_back({{"name", t.name}, {"description", t.description}, {"input_schema", t.parameters}});
    }
    if (!tools.empty()) tools.back()["cache_control"] = ephemeral;
    json body{{"model", request.model}, {"max_tokens", request.max_tokens}, {"stream", true}, {"messages", messages}};
    if (!request.system.empty()) {
        body["system"] = json::array({{{"type", "text"}, {"text", request.system}, {"cache_control", ephemeral}}});
    }
    if (!tools.empty()) body["tools"] = tools;
    return body;
}

std::unique_ptr<stream_parser> make_anthropic_parser(const stream_events& events) {
    return std::make_unique<parser>(events);
}

} // namespace ode
