#include "model/messages_codec.hpp"

#include <nlohmann/json.hpp>

#include <iostream>
#include <stdexcept>

int main() {
    using namespace lay;
    const auto require = [](bool condition, std::string_view explanation) {
        if (!condition) {
            throw std::runtime_error(std::string(explanation));
        }
    };
    const std::string question = "保留我的约束：只修改 config.txt，勿重发未确认的命令。";
    const auto tool = [](std::string id, std::string name, ToolResult result) {
        return ToolRecord{{std::move(id), std::move(name), {{"path", std::string("config.txt")}}},
                          true, std::move(result), 0};
    };
    auto denied = tool("denied", "edit", {ToolStatus::failed, "edit denied by user"});
    denied.dispatched = false;
    auto unknown = tool("unknown", "run", {ToolStatus::unknown, std::string(38000, 'z')});
    auto edit = tool("applied", "edit", {ToolStatus::ok, std::string(38000, 'x')});
    edit.edit = EditEvidence{"config.txt", "parent", "v1", "before", "after", "temporary", ""};
    const std::vector<Round> history{
        {"", {denied, unknown}}, {"", {edit}},
        {"", {tool("latest-read", "read", {ToolStatus::ok, "config.txt:4 settings", "version2"}),
              tool("latest-search", "search", {ToolStatus::skipped, "prior_tool_failed"})}},
    };
    const Request request{question, history, 24, true, true};
    messages::ContextLimits limits;
    limits.body_bytes = 8192;
    const auto result = messages::project(request, "fixture-model", limits);
    require(!result.body.empty() && result.body == messages::project(request, "fixture-model", limits).body,
            "projection is deterministic");
    require(result.layout.first_round == 2 && result.layout.retained_rounds == 1 &&
            result.layout.body_bytes == result.body.size() && result.body.size() <= limits.body_bytes,
            "old groups are dropped as a whole");
    const auto body = nlohmann::json::parse(result.body);
    const auto system = body.at("system").get<std::string>();
    require(result.layout.template_version == 4 && system.find("cite the exact line") != system.npos &&
            system.find("include the exact path:line separately") != system.npos &&
            system.find("call the edit tool before the final answer") != system.npos &&
            system.find("host's approval prompt") != system.npos,
            "Workspace requests require a precise source anchor and use host approval for requested edits");
    const auto& sent = body.at("messages");
    require(sent.size() == 3 && sent.at(0).at("content").at(0).at("text") == question,
            "the original Unicode question survives");
    const auto facts = sent.at(0).at("content").at(1).at("text").get<std::string>();
    require(facts.find("user denied this action") != facts.npos && facts.find("recorded as unknown") != facts.npos &&
            facts.find("after-sha256=after") != facts.npos, "summaries retain decisions and edit facts");
    const auto& calls = sent.at(1).at("content");
    const auto& observations = sent.at(2).at("content");
    require(calls.size() == 2 && observations.size() == 2, "latest group remains whole");
    for (std::size_t i = 0; i < calls.size(); ++i) {
        require(calls.at(i).at("id") == observations.at(i).at("tool_use_id"), "tool IDs and order remain paired");
    }
    limits.body_bytes = 300;
    const auto overflow = messages::project(request, "fixture-model", limits);
    require(overflow.body.empty() && overflow.diagnostic.find("latest complete tool group") != overflow.diagnostic.npos,
            "mandatory overflowing context gets an explicit explanation");

    const std::string follow_up = "保留上一次修改，这次只核对配置。";
    const std::vector<Task> tasks{{question, 0, 0, 0, true, SessionStop::end_turn}, {follow_up, 2, 2, 1, false}};
    const auto native = nlohmann::json{{"type", "reasoning"}, {"id", "rs_follow_up"}, {"encrypted_content", "opaque"}};
    const std::vector<Round> across_tasks{
        {"", {edit}}, {"Previous change is complete.", {}},
        {"", {tool("follow-up-read", "read", {ToolStatus::ok, "config.txt:4 current contents"})},
         {{native.dump(), 0, 0}}}
    };
    Provider responses;
    responses.protocol = Protocol::responses;
    responses.model = "fixture-model";
    const Request continued{follow_up, across_tasks, 24, false, false, tasks};
    const auto full = messages::project(continued, responses);
    const auto full_body = nlohmann::json::parse(full.body);
    const auto read_only = full_body.at("instructions").get<std::string>();
    require(read_only.find("cite the exact line") != read_only.npos &&
            read_only.find("latest user instruction") != read_only.npos &&
            read_only.find("call the edit tool") == read_only.npos &&
            full_body.at("tools").size() == 2,
            "Read-only follow-ups retain exact citations and only read and search tools");
    const auto& full_items = full_body.at("input");
    require(full_items.at(0).at("content") == question &&
            full_items.at(3).at("content") == "Previous change is complete." &&
            full_items.at(4).at("content") == follow_up && full_items.at(5) == native,
            "Responses history keeps the new user turn after the prior completed task");
    limits.body_bytes = 8192;
    const auto trimmed = messages::project(continued, responses, limits);
    const auto items = nlohmann::json::parse(trimmed.body).at("input");
    require(trimmed.layout.first_round > 0 && items.at(0).at("content").at(0).at("text") == question &&
            items.at(items.size() - 4).at("content") == follow_up && items.at(items.size() - 3) == native &&
            items.at(items.size() - 2).at("type") == "function_call" &&
            items.back().at("type") == "function_call_output" &&
            items.at(items.size() - 2).at("call_id") == items.back().at("call_id"),
            "Cross-task trimming preserves both user inputs and the complete current Responses tool group");

    auto early_read = tool("seed-read", "read", {ToolStatus::ok, "seed.txt:1 original", "original-version"});
    early_read.call.arguments.front().value = std::string("seed.txt");
    std::vector<Round> repeated_reads{{"", {early_read}}, {"", {denied}}};
    for (int i = 0; i < 36; ++i) {
        const auto sequence = std::to_string(i);
        repeated_reads.push_back({"", {tool("read-" + sequence, "read", {
            ToolStatus::ok, std::string(2000, 'x'), "revision-" + sequence})}});
    }
    const std::vector<Task> many_tasks{{question, 0, 0, 0, true, SessionStop::end_turn},
                                       {follow_up, repeated_reads.size(), 38, 0, false}};
    limits.body_bytes = 4096;
    const Request recall{follow_up, repeated_reads, 24, false, false, many_tasks};
    const auto compact = messages::project(recall, responses, limits);
    require(compact.layout.first_round > 2 && compact.body.size() <= limits.body_bytes &&
            compact.body.find("(read seed.txt): observation recorded, version=original-version") != compact.body.npos &&
            compact.body.find("user denied this action") != compact.body.npos &&
            compact.body.find("revision-35") != compact.body.npos,
            "Repeated reads cannot crowd the first file version, prior denials or the newest version out of history");

    messages::Stream anthropic;
    require(anthropic.push(
        R"(data: {"type":"message_start","message":{"role":"assistant","content":[],"stop_reason":null,"usage":{"input_tokens":100,"cache_read_input_tokens":900,"cache_creation_input_tokens":50,"output_tokens":1}}})"
        "\n\n"
        R"(data: {"type":"message_delta","delta":{"stop_reason":"end_turn"},"usage":{"output_tokens":12}})"
        "\n\n"), "Claude stream usage frames are accepted");
    require(anthropic.input_tokens() == 1050 && anthropic.output_tokens() == 12,
            "Claude input components are disjoint and streaming output counts are cumulative");

    messages::Stream chat(Protocol::chat_completions);
    require(chat.push(
        R"(data: {"choices":[],"usage":{"prompt_tokens":850,"input_tokens":1200,"completion_tokens":0,"prompt_tokens_details":{"cached_tokens":800},"cache_read_input_tokens":800}})"
        "\n\n"), "Chat usage-only frame is accepted");
    require(chat.input_tokens() == 850 && chat.output_tokens() == 0 &&
            chat.provider_usage()->at("prompt_tokens_details.cached_tokens") == 800,
            "Chat prompt tokens already include cache hits and explicit zero stays known");
    std::cout << "Deterministic context projection preserved constraints and complete tool groups.\n";
}
