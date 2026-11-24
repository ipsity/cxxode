#include "provider/codec.hpp"

namespace ode {
namespace {

class parser final : public stream_parser {
public:
    explicit parser(const stream_events& events) : events_(events) {}

    void event(std::string_view, const json& data) override {
        const auto type = data.value("type", "");
        if (type == "response.output_text.delta") {
            const auto text = data.value("delta", "");
            if (events_.text) events_.text(text);
        } else if (type == "response.reasoning_summary_text.delta" || type == "response.reasoning_text.delta") {
            if (events_.thinking) events_.thinking(data.value("delta", ""), "");
        } else if (type == "response.output_item.added") {
            const auto& item = data["item"];
            if (item.value("type", "") == "function_call" && events_.tool_started) events_.tool_started(item.value("name", ""));
        } else if (type == "response.output_item.done") {
            items_.push_back(data["item"]);
        } else if (type == "response.completed" || type == "response.incomplete") {
            completed_ = true;
            const auto& r = data["response"];
            incomplete_ = type == "response.incomplete";
            if (r.contains("usage") && r["usage"].is_object()) {
                const auto& u = r["usage"];
                reply_.tokens.input = u.value("input_tokens", std::size_t{});
                reply_.tokens.output = u.value("output_tokens", std::size_t{});
                if (u.contains("input_tokens_details") && u["input_tokens_details"].is_object()) {
                    reply_.tokens.cache_read = u["input_tokens_details"].value("cached_tokens", std::size_t{});
                    reply_.tokens.input -= std::min(reply_.tokens.input, reply_.tokens.cache_read);
                }
            }
        } else if (type == "response.failed" || type == "error") {
            const auto& e = type == "error" ? data : data["response"]["error"];
            reply_.error = e.is_object() ? e.value("message", e.dump()) : data.dump();
        }
    }

    model_reply finish() override {
        bool broken = false, calls = false;
        for (const auto& item : items_) {
            const auto kind = item.value("type", "");
            if (kind == "message") {
                std::string text;
                for (const auto& part : item.value("content", json::array())) {
                    if (part.value("type", "") == "output_text") text += part.value("text", "");
                    if (part.value("type", "") == "refusal") refused_ = true;
                }
                if (!text.empty()) reply_.content.blocks.push_back(block::make_text(text));
            } else if (kind == "function_call") {
                auto input = parse_arguments(item.value("arguments", ""));
                if (input.is_null()) broken = true;
                calls = true;
                reply_.content.blocks.push_back(block::make_tool_use(item.value("call_id", ""), item.value("name", ""), input));
            } else {
                // Reasoning and other items are replayed verbatim on the next request.
                block b;
                b.type = block::kind::opaque;
                b.name = "responses";
                b.input = item;
                reply_.content.blocks.push_back(std::move(b));
            }
        }
        reply_.complete = completed_ && !broken && reply_.error.empty();
        if (!reply_.complete) {
            reply_.stop = stop_reason::error;
            if (reply_.error.empty()) reply_.error = broken ? "Tool arguments were not valid JSON." : "The stream ended early.";
        } else if (calls) {
            reply_.stop = stop_reason::tool_use;
        } else if (refused_) {
            reply_.stop = stop_reason::refusal;
        } else {
            reply_.stop = incomplete_ ? stop_reason::max_tokens : stop_reason::end_turn;
        }
        return std::move(reply_);
    }

private:
    const stream_events& events_;
    model_reply reply_;
    std::vector<json> items_;
    bool completed_ = false;
    bool incomplete_ = false;
    bool refused_ = false;
};

} // namespace

json encode_responses(const model_request& request) {
    json input = json::array();
    for (const auto& m : request.messages) {
        const bool user = m.from == message::role::user;
        for (const auto& b : m.blocks) {
            switch (b.type) {
                case block::kind::text:
                    if (b.text.empty()) break;
                    input.push_back({{"role", user ? "user" : "assistant"},
                                     {"content", json::array({{{"type", user ? "input_text" : "output_text"}, {"text", b.text}}})}});
                    break;
                case block::kind::tool_use:
                    input.push_back({{"type", "function_call"}, {"call_id", b.id}, {"name", b.name}, {"arguments", b.input.dump()}});
                    break;
                case block::kind::tool_result:
                    input.push_back({{"type", "function_call_output"}, {"call_id", b.id}, {"output", b.text}});
                    break;
                case block::kind::opaque:
                    if (b.name == "responses") input.push_back(b.input);
                    break;
                case block::kind::thinking: break;
            }
        }
    }
    json tools = json::array();
    for (const auto& t : request.tools) {
        tools.push_back({{"type", "function"}, {"name", t.name}, {"description", t.description}, {"parameters", t.parameters}});
    }
    json body{{"model", request.model}, {"stream", true}, {"input", input}, {"store", false},
              {"max_output_tokens", request.max_tokens}, {"include", json::array({"reasoning.encrypted_content"})}};
    if (!request.system.empty()) body["instructions"] = request.system;
    if (!tools.empty()) body["tools"] = tools;
    return body;
}

std::unique_ptr<stream_parser> make_responses_parser(const stream_events& events) {
    return std::make_unique<parser>(events);
}

} // namespace ode
