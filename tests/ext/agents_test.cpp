#include "config/config.hpp"
#include "ext/agents.hpp"

#include <doctest.h>

#include <algorithm>
#include <deque>
#include <fstream>
#include <random>

using namespace ode;

namespace {

struct workspace {
    fs::path path = fs::temp_directory_path() / ("ode-subagent-" + std::to_string(std::random_device{}()));

    workspace() { fs::create_directories(path / ".claude" / "agents"); }
    ~workspace() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

class subagent_provider final : public provider {
public:
    std::deque<model_reply> replies;
    std::vector<model_request> requests;

    model_reply send(const model_request& request, const stream_events&, cancel_token&) override {
        requests.push_back(request);
        REQUIRE_FALSE(replies.empty());
        auto result = std::move(replies.front());
        replies.pop_front();
        return result;
    }

    void respond(std::vector<block> blocks, stop_reason stop) {
        model_reply response;
        response.content.blocks = std::move(blocks);
        response.stop = stop;
        response.complete = true;
        replies.push_back(std::move(response));
    }
};

} // namespace

TEST_CASE("Task reads agent frontmatter and forwards permissions to the caller") {
    workspace ws;
    std::ofstream(ws.path / ".claude" / "agents" / "review.md", std::ios::binary) <<
        "---\nname: fixture-reviewer\ndescription: Review a file\nmodel: haiku\ntools:\n"
        "  - Bash\n  - mcp__fixture__*\n---\nReview instructions.\n";
    auto agents = load_agents(ws.path);
    const auto found = std::ranges::find(agents, "fixture-reviewer", &agent_config::name);
    REQUIRE(found != agents.end());
    CHECK(found->tools == std::vector<std::string>{"Bash", "mcp__fixture__*"});

    auto tools = builtin_tools();
    tools.push_back({"mcp__fixture__read", "fixture", {{"type", "object"}}, tool_access::read, {}, {}});
    tools.push_back({"mcp__elsewhere__read", "other fixture", {{"type", "object"}}, tool_access::read, {}, {}});
    permission_rules rules;
    subagent_provider provider;
    provider.respond({block::make_text("Intermediate."), block::make_tool_use("b", "Bash", {{"command", "fixture-command"}})},
                     stop_reason::tool_use);
    provider.respond({block::make_text("Final review.")}, stop_reason::end_turn);
    auto task = make_task_tools({*found}, provider, std::move(tools), rules, [] { return "parent-model"; });
    REQUIRE(task.size() == 1);
    CHECK(task.front().name == "Task");
    CHECK(task.front().description.find("fixture-reviewer: Review a file") != std::string::npos);
    CHECK(task.front().schema["required"] == json::array({"subagent_type", "prompt"}));
    CHECK(task.front().schema["properties"].contains("description"));

    tool_context ctx;
    ctx.root = ws.path;
    int asks = 0;
    ctx.ask = [&](const tool& t, const json& input) {
        ++asks;
        CHECK(t.name == "Bash");
        CHECK(input["command"] == "fixture-command");
        return answer::no;
    };
    const auto result = task.front().run({{"subagent_type", "fixture-reviewer"}, {"prompt", "review"}}, ctx);
    CHECK(asks == 1);
    CHECK_FALSE(result.is_error);
    CHECK(result.text == "Final review.");
    REQUIRE(provider.requests.size() == 2);
    CHECK(provider.requests[0].model == resolve_model_alias("haiku"));
    CHECK(provider.requests[0].system == "Review instructions.\n");
    REQUIRE(provider.requests[0].tools.size() == 2);
    CHECK(provider.requests[0].tools[0].name == "Bash");
    CHECK(provider.requests[0].tools[1].name == "mcp__fixture__read");
    CHECK(provider.requests[1].messages.back().blocks.front().is_error);
}

TEST_CASE("Task inherits the current parent model and shares the turn's file backup") {
    workspace ws;
    std::ofstream(ws.path / ".claude" / "agents" / "fixture-writer.md") <<
        "---\nmodel: inherit\ntools: Read, Write\n---\nWrite instructions.\n";
    const auto agents = load_agents(ws.path);
    const auto found = std::ranges::find(agents, "fixture-writer", &agent_config::name);
    REQUIRE(found != agents.end());
    CHECK(found->tools == std::vector<std::string>{"Read", "Write"});

    subagent_provider provider;
    provider.respond({block::make_tool_use("w", "Write", {{"file_path", "new.txt"}, {"content", "new"}})}, stop_reason::tool_use);
    provider.respond({block::make_text("Wrote file.")}, stop_reason::end_turn);
    permission_rules rules;
    std::string model = "old-model";
    auto tasks = make_task_tools({*found}, provider, builtin_tools(), rules, [&] { return model; });
    model = "current-model";
    tool_context ctx;
    ctx.root = ws.path;
    turn_backup backup;
    ctx.backup = &backup;
    const auto result = tasks.front().run({{"subagent_type", "fixture-writer"}, {"prompt", "write file"}}, ctx);
    CHECK_FALSE(result.is_error);
    REQUIRE_FALSE(provider.requests.empty());
    CHECK(provider.requests[0].model == "current-model");
    CHECK(read_file(ws.path / "new.txt") == "new");
    CHECK(restore(backup, ws.path) == std::vector<std::string>{"new.txt"});
    CHECK_FALSE(fs::exists(ws.path / "new.txt"));
}
