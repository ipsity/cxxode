#include "core/agent.hpp"
#include "core/permission.hpp"
#include "core/session.hpp"

#include <doctest.h>

#include <deque>
#include <fstream>
#include <random>

using namespace lay;

namespace {

// Replays canned replies and records every request it received.
class scripted_provider final : public provider {
public:
    std::deque<model_reply> replies;
    std::vector<model_request> requests;

    model_reply send(const model_request& request, const stream_events& events, cancel_token&) override {
        requests.push_back(request);
        if (replies.empty()) return {};
        auto r = std::move(replies.front());
        replies.pop_front();
        if (events.text) events.text(r.content.plain_text());
        return r;
    }
};

model_reply reply(std::vector<block> blocks, stop_reason stop, bool complete = true) {
    model_reply r;
    r.content.blocks = std::move(blocks);
    r.stop = stop;
    r.complete = complete;
    return r;
}

struct temp_dir {
    fs::path path;
    temp_dir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("lay-agent-" + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~temp_dir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

const tool& find(const std::vector<tool>& tools, std::string_view name) {
    for (const auto& t : tools) if (t.name == name) return t;
    throw std::runtime_error("no tool");
}

} // namespace

TEST_CASE("loop runs tools until the model stops and undo restores files") {
    temp_dir dir;
    std::ofstream(dir.path / "a.txt", std::ios::binary) << "old\n";
    scripted_provider p;
    p.replies.push_back(reply({block::make_text("Reading."), block::make_tool_use("1", "Read", {{"file_path", "a.txt"}}),
                               block::make_tool_use("2", "Glob", {{"pattern", "*.txt"}})},
                              stop_reason::tool_use));
    p.replies.push_back(reply({block::make_tool_use("3", "Edit", {{"file_path", "a.txt"}, {"old_string", "old"}, {"new_string", "new"}})},
                              stop_reason::tool_use));
    p.replies.push_back(reply({block::make_text("Done.")}, stop_reason::end_turn));

    permission_rules rules;
    agent a(p, builtin_tools(), rules, dir.path);
    agent_events ev;
    std::vector<std::string> started;
    ev.tool_started = [&](const tool& t, const json&) { started.push_back(t.name); };
    cancel_token cancel;
    auto r = a.run("change it", ev, cancel);
    CHECK(r.stop == stop_reason::end_turn);
    CHECK(p.requests.size() == 3);
    CHECK(started.size() == 3);
    // Second request carries both parallel read results, in call order.
    const auto& results = p.requests[1].messages.back().blocks;
    REQUIRE(results.size() == 2);
    CHECK(results[0].id == "1");
    CHECK(results[1].id == "2");
    CHECK(*read_file(dir.path / "a.txt") == "new\n");
    REQUIRE(a.can_undo());
    CHECK(a.undo() == std::vector<std::string>{"a.txt"});
    CHECK(*read_file(dir.path / "a.txt") == "old\n");
}

TEST_CASE("incomplete reply dispatches nothing") {
    temp_dir dir;
    scripted_provider p;
    p.replies.push_back(reply({block::make_tool_use("1", "Write", {{"file_path", "x.txt"}, {"content", "x"}})},
                              stop_reason::error, false));
    permission_rules rules;
    rules.mode = permission_mode::yolo;
    agent a(p, builtin_tools(), rules, dir.path);
    agent_events ev;
    cancel_token cancel;
    auto r = a.run("write", ev, cancel);
    CHECK(r.stop == stop_reason::error);
    CHECK_FALSE(fs::exists(dir.path / "x.txt"));
}

TEST_CASE("declined bash is reported to the model and always adds a rule") {
    temp_dir dir;
    scripted_provider p;
    p.replies.push_back(reply({block::make_tool_use("1", "Bash", {{"command", "npm run build"}})}, stop_reason::tool_use));
    p.replies.push_back(reply({block::make_tool_use("2", "Bash", {{"command", "npm run build && npm run test"}})}, stop_reason::tool_use));
    p.replies.push_back(reply({block::make_text("ok")}, stop_reason::end_turn));
    permission_rules rules;
    agent a(p, builtin_tools(), rules, dir.path);
    agent_events ev;
    std::vector<answer> answers{answer::no, answer::always};
    int asked = 0;
    ev.ask = [&](const tool&, const json&) { return answers[static_cast<std::size_t>(asked++)]; };
    cancel_token cancel;
    a.run("build", ev, cancel);
    CHECK(asked == 2);
    CHECK(p.requests[1].messages.back().blocks[0].is_error);
    REQUIRE(rules.allow.size() == 1);
    CHECK(rules.allow[0] == "Bash(npm run:*)");
}

TEST_CASE("steering typed during work reaches the next request") {
    temp_dir dir;
    scripted_provider p;
    p.replies.push_back(reply({block::make_tool_use("1", "Glob", {{"pattern", "*"}})}, stop_reason::tool_use));
    p.replies.push_back(reply({block::make_text("ok")}, stop_reason::end_turn));
    permission_rules rules;
    agent a(p, builtin_tools(), rules, dir.path);
    agent_events ev;
    bool given = false;
    ev.take_steering = [&] {
        if (given) return std::vector<std::string>{};
        given = true;
        return std::vector<std::string>{"also check tests"};
    };
    cancel_token cancel;
    a.run("go", ev, cancel);
    const auto& last = p.requests[1].messages.back();
    CHECK(last.blocks.back().text.find("also check tests") != std::string::npos);
}

TEST_CASE("permission rules") {
    const auto tools = builtin_tools();
    const auto& bash = find(tools, "Bash");
    const auto& edit = find(tools, "Edit");
    const auto& read = find(tools, "Read");
    const fs::path root = fs::temp_directory_path() / "ws";
    permission_rules r;
    CHECK(r.check(read, {{"file_path", "x"}}, root) == verdict::allow);
    CHECK(r.check(edit, {{"file_path", "x"}}, root) == verdict::allow);
    CHECK(r.check(edit, {{"file_path", (fs::temp_directory_path() / "elsewhere.txt").string()}}, root) == verdict::ask);
    CHECK(r.check(bash, {{"command", "git status && ls -la"}}, root) == verdict::allow);
    CHECK(r.check(bash, {{"command", "ls > out.txt"}}, root) == verdict::ask);
    CHECK(r.check(bash, {{"command", "rm -rf build"}}, root) == verdict::ask);
    r.allow.push_back("Bash(cmake --build:*)");
    CHECK(r.check(bash, {{"command", "cmake --build build/dev"}}, root) == verdict::allow);
    CHECK(r.check(bash, {{"command", "cmake --build build && rm x"}}, root) == verdict::ask);
    r.deny.push_back("Bash(git push:*)");
    r.mode = permission_mode::yolo;
    CHECK(r.check(bash, {{"command", "git push origin main"}}, root) == verdict::deny);
    CHECK(r.check(bash, {{"command", "rm -rf build"}}, root) == verdict::allow);
    r.mode = permission_mode::plan;
    CHECK(r.check(edit, {{"file_path", "x"}}, root) == verdict::deny);
    CHECK(r.check(read, {{"file_path", "x"}}, root) == verdict::allow);
    CHECK(command_segments("a 'b;c' | d && e") == std::vector<std::string>{"a 'b;c'", "d", "e"});
}

TEST_CASE("session log round trip drops a torn line and closes dangling calls") {
    temp_dir dir;
    const auto path = dir.path / "s.jsonl";
    {
        session_log log;
        REQUIRE(log.open(path));
        log.append_message({message::role::user, {block::make_text("hi")}});
        log.append_message({message::role::assistant, {block::make_tool_use("t", "Read", {{"file_path", "a"}})}});
    }
    std::ofstream(path, std::ios::app | std::ios::binary) << "{\"type\":\"message\",\"mess";
    auto s = load_session(path);
    REQUIRE(s.messages.size() == 3);
    CHECK(s.messages[2].blocks[0].type == block::kind::tool_result);
    CHECK(s.messages[2].blocks[0].id == "t");
}
