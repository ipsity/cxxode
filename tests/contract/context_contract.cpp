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
    std::cout << "Deterministic context projection preserved constraints and complete tool groups.\n";
}
