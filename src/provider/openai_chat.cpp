#include "provider/codec.hpp"

#include <map>

namespace lay {
namespace {

class parser final : public stream_parser {
public:
    explicit parser(const stream_events& events) : events_(events) {}

    void event(std::string_view, const json& data) override {
        if (data.contains("error")) {
            error_ = data["error"].value("message", data["error"].dump());
            return;
        }
        if (data.contains("usage") && data["usage"].is_object()) {
            const auto& u = data["usage"];
            reply_.tokens.input = u.value("prompt_tokens", std::size_t{});
            reply_.tokens.output = u.value("completion_tokens", std::size_t{});
            if (u.contains("prompt_tokens_details") && u["prompt_tokens_details"].is_object()) {
                reply_.tokens.cache_read = u["prompt_tokens_details"].value("cached_tokens", std::size_t{});
                reply_.tokens.input -= std::min(reply_.tokens.input, reply_.tokens.cache_read);
            }
        }
        if (!data.contains("choices") || data["choices"].empty()) return;
        const auto& choice = data["choices"][0];
        if (choice.contains("delta")) {
            const auto& d = choice["delta"];
            if (d.contains("content") && d["content"].is_string()) {
                const auto text = d["content"].get<std::string>();
                text_ += text;
                if (events_.text) events_.text(text);
            }
            for (const auto& key : {"reasoning_content", "reasoning"}) {
                if (d.contains(key) && d[key].is_string() && events_.thinking) events_.thinking(d[key].get<std::string>(), "");
            }
            if (d.contains("tool_calls")) {
                for (const auto& call : d["tool_calls"]) {
                    auto& slot = calls_[call.value("index", 0)];
                    if (call.contains("id") && call["id"].is_string()) slot.id = call["id"];
                    if (call.contains("function")) {
                        const auto& f = call["function"];
                        if (f.contains("name") && f["name"].is_string() && slot.name.empty()) {
                            slot.name = f["name"];
                            if (events_.tool_started) events_.tool_started(slot.name);
                        }
                        if (f.contains("arguments") && f["arguments"].is_string()) slot.arguments += f["arguments"].get<std::string>();
                    }
                }
            }
        }
        if (choice.contains("finish_reason") && choice["finish_reason"].is_string()) finish_ = choice["finish_reason"];
    }

    model_reply finish() override {
        if (!text_.empty()) reply_.content.blocks.push_back(block::make_text(text_));
        bool broken = false;
        for (auto& [index, call] : calls_) {
            auto input = parse_arguments(call.arguments);
            if (input.is_null()) broken = true;
            reply_.content.blocks.push_back(block::make_tool_use(call.id, call.name, std::move(input)));
        }
        // Some servers omit [DONE]; a finish_reason is then the end marker.
        reply_.complete = !finish_.empty() && !broken && error_.empty();
        reply_.error = error_;
        if (!reply_.complete) {
            reply_.stop = stop_reason::error;
            if (reply_.error.empty()) reply_.error = broken ? "Tool arguments were not valid JSON." : "The stream ended early.";
        } else if (!calls_.empty()) {
            reply_.stop = stop_reason::tool_use;
        } else if (finish_ == "length") {
            reply_.stop = stop_reason::max_tokens;
        } else if (finish_ == "content_filter") {
            reply_.stop = stop_reason::refusal;
        } else {
            reply_.stop = stop_reason::end_turn;
        }
        return std::move(reply_);
    }

private:
    struct call { std::string id, name, arguments; };
    const stream_events& events_;
    model_reply reply_;
    std::string text_;
    std::map<int, call> calls_;
    std::string finish_;
    std::string error_;
};

} // namespace

json encode_chat(const model_request& request) {
    json messages = json::array();
    if (!request.system.empty()) messages.push_back({{"role", "system"}, {"content", request.system}});
    for (const auto& m : request.messages) {
        if (m.from == message::role::assistant) {
            json item{{"role", "assistant"}, {"content", m.plain_text()}};
            json calls = json::array();
            for (const auto* b : m.tool_uses()) {
                calls.push_back({{"id", b->id}, {"type", "function"},
                                 {"function", {{"name", b->name}, {"arguments", b->input.dump()}}}});
            }
            if (!calls.empty()) item["tool_calls"] = calls;
            messages.push_back(std::move(item));
            continue;
        }
        std::string text;
        for (const auto& b : m.blocks) {
            if (b.type == block::kind::tool_result) {
                messages.push_back({{"role", "tool"}, {"tool_call_id", b.id}, {"content", b.text}});
            } else if (b.type == block::kind::text) {
                text += b.text;
            }
        }
        if (!text.empty()) messages.push_back({{"role", "user"}, {"content", text}});
    }
    json body{{"model", request.model}, {"stream", true}, {"messages", messages},
              {"max_tokens", request.max_tokens}, {"stream_options", {{"include_usage", true}}}};
    json tools = json::array();
    for (const auto& t : request.tools) {
        tools.push_back({{"type", "function"},
                         {"function", {{"name", t.name}, {"description", t.description}, {"parameters", t.parameters}}}});
    }
    if (!tools.empty()) body["tools"] = tools;
    return body;
}

std::unique_ptr<stream_parser> make_chat_parser(const stream_events& events) {
    return std::make_unique<parser>(events);
}

} // namespace lay
