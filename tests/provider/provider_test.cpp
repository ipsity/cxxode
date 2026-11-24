#include "provider/provider.hpp"
#include "provider/sse.hpp"

#include <doctest.h>

#include <string>
#include <vector>

using namespace lay;

TEST_CASE("sse decoder handles arbitrary splits") {
    const std::string stream = "event: a\r\ndata: {\"x\":1}\r\n\r\n: comment\ndata: one\ndata: two\n\n";
    for (std::size_t step = 1; step <= stream.size(); ++step) {
        std::vector<std::string> seen;
        sse_decoder d([&](std::string_view e, std::string_view data) { seen.push_back(std::string(e) + "|" + std::string(data)); });
        for (std::size_t i = 0; i < stream.size(); i += step) d.feed(std::string_view(stream).substr(i, step));
        d.finish();
        REQUIRE(seen.size() == 2);
        CHECK(seen[0] == "a|{\"x\":1}");
        CHECK(seen[1] == "message|one\ntwo");
    }
}

namespace {

const char* anthropic_tool_stream =
    "event: message_start\ndata: {\"type\":\"message_start\",\"message\":{\"usage\":{\"input_tokens\":10,\"output_tokens\":1}}}\n\n"
    "data: {\"type\":\"content_block_start\",\"index\":0,\"content_block\":{\"type\":\"text\",\"text\":\"\"}}\n\n"
    "data: {\"type\":\"content_block_delta\",\"index\":0,\"delta\":{\"type\":\"text_delta\",\"text\":\"Hi \"}}\n\n"
    "data: {\"type\":\"content_block_stop\",\"index\":0}\n\n"
    "data: {\"type\":\"content_block_start\",\"index\":1,\"content_block\":{\"type\":\"tool_use\",\"id\":\"t1\",\"name\":\"read\",\"input\":{}}}\n\n"
    "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"{\\\"path\\\":\"}}\n\n"
    "data: {\"type\":\"content_block_delta\",\"index\":1,\"delta\":{\"type\":\"input_json_delta\",\"partial_json\":\"\\\"a.txt\\\"}\"}}\n\n"
    "data: {\"type\":\"content_block_stop\",\"index\":1}\n\n"
    "data: {\"type\":\"message_delta\",\"delta\":{\"stop_reason\":\"tool_use\"},\"usage\":{\"output_tokens\":7}}\n\n";

} // namespace

TEST_CASE("anthropic stream with tool call") {
    std::string shown;
    stream_events ev;
    ev.text = [&](std::string_view t) { shown += t; };
    const std::string full = std::string(anthropic_tool_stream) + "data: {\"type\":\"message_stop\"}\n\n";
    auto r = decode_stream(protocol::anthropic, full, ev);
    CHECK(r.complete);
    CHECK(r.stop == stop_reason::tool_use);
    CHECK(shown == "Hi ");
    CHECK(r.tokens.input == 10);
    CHECK(r.tokens.output == 7);
    auto calls = r.content.tool_uses();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0]->name == "read");
    CHECK(calls[0]->input["path"] == "a.txt");
}

TEST_CASE("anthropic stream without message_stop is incomplete") {
    auto r = decode_stream(protocol::anthropic, anthropic_tool_stream);
    CHECK_FALSE(r.complete);
    CHECK(r.stop == stop_reason::error);
}

TEST_CASE("chat completions stream with split tool arguments") {
    const char* s =
        "data: {\"choices\":[{\"delta\":{\"content\":\"ok\"}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"c1\",\"function\":{\"name\":\"bash\",\"arguments\":\"{\\\"command\\\"\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"function\":{\"arguments\":\":\\\"ls\\\"}\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n"
        "data: {\"choices\":[],\"usage\":{\"prompt_tokens\":5,\"completion_tokens\":3}}\n\n"
        "data: [DONE]\n\n";
    auto r = decode_stream(protocol::openai_chat, s);
    CHECK(r.complete);
    CHECK(r.stop == stop_reason::tool_use);
    CHECK(r.content.plain_text() == "ok");
    auto calls = r.content.tool_uses();
    REQUIRE(calls.size() == 1);
    CHECK(calls[0]->input["command"] == "ls");
    CHECK(r.tokens.output == 3);
}

TEST_CASE("chat stream with broken arguments is not dispatchable") {
    const char* s =
        "data: {\"choices\":[{\"delta\":{\"tool_calls\":[{\"index\":0,\"id\":\"c1\",\"function\":{\"name\":\"bash\",\"arguments\":\"{\\\"comm\"}}]}}]}\n\n"
        "data: {\"choices\":[{\"delta\":{},\"finish_reason\":\"tool_calls\"}]}\n\n";
    auto r = decode_stream(protocol::openai_chat, s);
    CHECK_FALSE(r.complete);
}

TEST_CASE("responses stream") {
    const char* s =
        "event: response.output_text.delta\ndata: {\"type\":\"response.output_text.delta\",\"delta\":\"he\"}\n\n"
        "data: {\"type\":\"response.output_item.done\",\"item\":{\"type\":\"reasoning\",\"id\":\"r1\",\"encrypted_content\":\"x\"}}\n\n"
        "data: {\"type\":\"response.output_item.done\",\"item\":{\"type\":\"message\",\"content\":[{\"type\":\"output_text\",\"text\":\"hello\"}]}}\n\n"
        "data: {\"type\":\"response.output_item.done\",\"item\":{\"type\":\"function_call\",\"call_id\":\"f1\",\"name\":\"glob\",\"arguments\":\"{\\\"pattern\\\":\\\"*.cpp\\\"}\"}}\n\n"
        "data: {\"type\":\"response.completed\",\"response\":{\"usage\":{\"input_tokens\":9,\"output_tokens\":4}}}\n\n";
    auto r = decode_stream(protocol::openai_responses, s);
    CHECK(r.complete);
    CHECK(r.stop == stop_reason::tool_use);
    CHECK(r.content.plain_text() == "hello");
    REQUIRE(r.content.blocks.size() == 3);
    CHECK(r.content.blocks[0].type == block::kind::opaque);

    model_request req;
    req.model = "m";
    req.messages.push_back(r.content);
    auto body = encode_request(protocol::openai_responses, req);
    CHECK(body["input"][0]["type"] == "reasoning");
    CHECK(body["input"][2]["type"] == "function_call");
}

TEST_CASE("anthropic request encoding pairs tool use and result") {
    model_request req;
    req.model = "claude";
    req.system = "sys";
    req.tools.push_back({"read", "Read a file", {{"type", "object"}}});
    message a{message::role::assistant, {block::make_tool_use("t1", "read", {{"path", "x"}})}};
    message u{message::role::user, {block::make_tool_result("t1", "content", false)}};
    req.messages = {message{message::role::user, {block::make_text("hi")}}, a, u};
    auto body = encode_request(protocol::anthropic, req);
    CHECK(body["messages"].size() == 3);
    CHECK(body["messages"][2]["content"][0]["tool_use_id"] == "t1");
    CHECK(body["messages"][2]["content"][0].contains("cache_control"));
    CHECK(body["tools"][0]["input_schema"]["type"] == "object");
    CHECK(body["system"][0]["text"] == "sys");
}
