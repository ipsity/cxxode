#include "core/message.hpp"

namespace ode {
namespace {

constexpr const char* kind_names[] = {"text", "tool_use", "tool_result", "thinking", "opaque"};

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
        m.blocks.push_back(std::move(b));
    }
    return m;
}

} // namespace ode
