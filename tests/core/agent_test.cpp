#include "core/agent.hpp"
#include "core/permission.hpp"
#include "core/session.hpp"
#include "ext/lua/host.hpp"

#include <doctest.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <fstream>
#include <random>
#include <thread>

using namespace ode;

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
        path = fs::temp_directory_path() / ("ode-agent-" + std::to_string(rd()));
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
    std::map<std::string, std::chrono::milliseconds> elapsed;
    ev.tool_finished = [&](const tool& t, const json&, const tool_output& out) {
        if (out.elapsed) elapsed.emplace(t.name, *out.elapsed);
    };
    cancel_token cancel;
    auto r = a.run("change it", ev, cancel);
    CHECK(r.stop == stop_reason::end_turn);
    CHECK(p.requests.size() == 3);
    CHECK(started.size() == 3);
    CHECK(elapsed.size() == 3);
    CHECK(elapsed.contains("Read"));
    CHECK(elapsed.contains("Glob"));
    CHECK(elapsed.contains("Edit"));
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

TEST_CASE("invalid built-in tool input is rejected before hooks and permissions") {
    temp_dir dir;
    scripted_provider p;
    p.replies.push_back(reply({
        block::make_tool_use("missing", "Write", {{"file_path", "x.txt"}}),
        block::make_tool_use("type", "Bash", {{"command", "echo ok"}, {"timeout", "fast"}}),
        block::make_tool_use("enum", "Grep", {{"pattern", "x"}, {"output_mode", "all"}}),
        block::make_tool_use("extra", "Read", {{"file_path", "x.txt"}, {"extra", true}}),
    }, stop_reason::tool_use));
    p.replies.push_back(reply({block::make_text("Done.")}, stop_reason::end_turn));
    permission_rules rules;
    bool hook_called = false;
    agent_hooks hooks;
    hooks.pre_tool = [&](const tool&, const json&, cancel_token&) {
        hook_called = true;
        return hook_verdict{};
    };
    agent a(p, builtin_tools(), rules, dir.path, hooks);
    agent_events ev;
    bool asked = false;
    ev.ask = [&](const tool&, const json&) { asked = true; return answer::no; };
    cancel_token cancel;
    CHECK(a.run("validate", ev, cancel).stop == stop_reason::end_turn);
    CHECK_FALSE(hook_called);
    CHECK_FALSE(asked);
    CHECK_FALSE(fs::exists(dir.path / "x.txt"));
    const auto& results = p.requests.at(1).messages.back().blocks;
    REQUIRE(results.size() == 4);
    const std::string_view fields[]{"content", "timeout", "output_mode", "extra"};
    for (std::size_t i = 0; i < results.size(); ++i) {
        CHECK(results[i].is_error);
        CHECK(results[i].text.find(fields[i]) != std::string::npos);
    }
}

TEST_CASE("deferred tools are declared after tool_search and then use the normal agent dispatch") {
    temp_dir dir;
    scripted_provider model;
    model.replies.push_back(reply({block::make_tool_use("search", "tool_search", {{"query", "hidden lookup"}})},
                                  stop_reason::tool_use));
    model.replies.push_back(reply({block::make_tool_use("hidden", "mcp__fixture__hidden", {{"query", "value"}})},
                                  stop_reason::tool_use));
    model.replies.push_back(reply({block::make_text("Done.")}, stop_reason::end_turn));

    tool direct{"Direct", "Direct fixture", {{"type", "object"}}, tool_access::read,
        [](const json&, tool_context&) { return tool_output{"direct", false, {}, {}}; }, {}};
    tool deferred{"mcp__fixture__hidden", "Hidden lookup", {{"type", "object"}}, tool_access::read,
        [](const json& input, tool_context&) { return tool_output{input.value("query", std::string{}), false, {}, {}}; }, {}};
    deferred.exposure = tool_exposure::deferred;

    permission_rules rules;
    agent core(model, {direct, deferred, make_tool_search_tool()}, rules, dir.path);
    agent_events events;
    cancel_token cancel;
    CHECK(core.run("search and call", events, cancel).stop == stop_reason::end_turn);
    REQUIRE(model.requests.size() == 3);
    CHECK(std::ranges::any_of(model.requests[0].tools, [](const auto& item) { return item.name == "Direct"; }));
    CHECK(std::ranges::any_of(model.requests[0].tools, [](const auto& item) { return item.name == "tool_search"; }));
    CHECK_FALSE(std::ranges::any_of(model.requests[0].tools, [](const auto& item) { return item.name == "mcp__fixture__hidden"; }));
    CHECK(std::ranges::any_of(model.requests[1].tools, [](const auto& item) { return item.name == "mcp__fixture__hidden"; }));
    CHECK(model.requests[2].messages.back().blocks.front().text == "value");
}

TEST_CASE("CodeMode nested writes use tool hooks, permissions, and turn undo") {
    temp_dir dir;
    scripted_provider model;
    model.replies.push_back(reply({block::make_tool_use("code", "codemode", {
        {"code", "local result = tools.call('Write', {file_path='nested.txt', content='written'}); return result.text"}})},
        stop_reason::tool_use));
    model.replies.push_back(reply({block::make_text("Done.")}, stop_reason::end_turn));

    lua_host lua;
    auto tools = builtin_tools();
    tools.push_back(lua.code_mode_tool());
    permission_rules rules;
    int write_hooks = 0;
    agent_hooks hooks;
    hooks.pre_tool = [&](const tool& candidate, const json&, cancel_token&) {
        hook_verdict result;
        if (candidate.name == "Write") {
            ++write_hooks;
            result.tool_permission = hook_verdict::permission::ask;
        }
        return result;
    };
    agent core(model, std::move(tools), rules, dir.path, hooks);
    agent_events events;
    std::vector<std::string> asked;
    events.ask = [&](const tool& candidate, const json&) { asked.push_back(candidate.name); return answer::yes; };
    cancel_token cancel;

    CHECK(core.run("write through CodeMode", events, cancel).stop == stop_reason::end_turn);
    CHECK(read_file(dir.path / "nested.txt") == std::optional<std::string>{"written"});
    CHECK(asked == std::vector<std::string>{"Write"});
    CHECK(write_hooks == 1);
    CHECK(core.undo() == std::vector<std::string>{"nested.txt"});
    CHECK_FALSE(fs::exists(dir.path / "nested.txt"));
}

TEST_CASE("CodeMode batches independent MCP tools concurrently and preserves order") {
    temp_dir dir;
    scripted_provider model;
    model.replies.push_back(reply({block::make_tool_use("code", "codemode", {{"code", "run batch"}})},
                                  stop_reason::tool_use));
    model.replies.push_back(reply({block::make_text("Done.")}, stop_reason::end_turn));

    std::atomic<int> active{0};
    std::atomic<int> maximum{0};
    const auto make_mcp = [&](std::string name) {
        tool result{name, "MCP fixture", {{"type", "object"}}, tool_access::execute,
            [&, name](const json&, tool_context&) {
                const int now = ++active;
                auto seen = maximum.load();
                while (seen < now && !maximum.compare_exchange_weak(seen, now)) {}
                std::this_thread::sleep_for(std::chrono::milliseconds(50));
                --active;
                return tool_output{name, false, {}, {}};
            }, {}};
        return result;
    };
    tool codemode{"codemode", "fixture CodeMode", {{"type", "object"}}, tool_access::read,
        [](const json&, tool_context& context) {
            const auto results = context.invoke_tools({
                {"mcp__one__read", json::object()}, {"mcp__two__read", json::object()}
            }, context.deadline);
            return tool_output{results[0].text + "," + results[1].text, false, {}, {}};
        }, {}};
    codemode.delegates_tool_permissions = true;
    permission_rules rules;
    agent core(model, {make_mcp("mcp__one__read"), make_mcp("mcp__two__read"), codemode}, rules, dir.path);
    agent_events events;
    events.ask = [](const tool&, const json&) { return answer::yes; };
    cancel_token cancel;

    CHECK(core.run("batch MCP tools", events, cancel).stop == stop_reason::end_turn);
    REQUIRE(model.requests.size() == 2);
    CHECK(maximum.load() == 2);
    CHECK(model.requests[1].messages.back().blocks.front().text == "mcp__one__read,mcp__two__read");
}

TEST_CASE("parallel tool batches are bounded and preserve result order") {
    temp_dir dir;
    scripted_provider p;
    std::vector<block> calls;
    for (int i = 0; i < 9; ++i) calls.push_back(block::make_tool_use(std::to_string(i), "Fixture", json::object()));
    p.replies.push_back(reply(std::move(calls), stop_reason::tool_use));
    p.replies.push_back(reply({block::make_text("Done.")}, stop_reason::end_turn));
    permission_rules rules;
    std::atomic<int> active{0};
    std::atomic<bool> exceeded_limit{false};
    tool fixture{"Fixture", {}, {}, tool_access::read, [&](const json&, tool_context&) {
        if (++active > 4) exceeded_limit = true;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        --active;
        return tool_output{"ok", false, {}, {}};
    }, {}};
    agent a(p, {fixture}, rules, dir.path);
    agent_events ev;
    cancel_token cancel;
    CHECK(a.run("run batch", ev, cancel).stop == stop_reason::end_turn);
    CHECK_FALSE(exceeded_limit.load());
    const auto& results = p.requests.at(1).messages.back().blocks;
    REQUIRE(results.size() == 9);
    for (std::size_t i = 0; i < results.size(); ++i) {
        CHECK(results[i].id == std::to_string(i));
        CHECK(results[i].text == "ok");
    }
}

TEST_CASE("independent built-in writes overlap and undo preserves the first backup") {
    temp_dir dir;
    std::ofstream(dir.path / "a.txt") << "old";
    scripted_provider p;
    p.replies.push_back(reply({
        block::make_tool_use("read", "Read", {{"file_path", "a.txt"}}),
        block::make_tool_use("edit", "Edit", {{"file_path", "a.txt"}, {"old_string", "old"}, {"new_string", "first"}}),
        block::make_tool_use("write", "Write", {{"file_path", "b.txt"}, {"content", "new"}}),
        block::make_tool_use("edit-again", "Edit", {{"file_path", "./A.TXT"}, {"old_string", "first"}, {"new_string", "last"}}),
    }, stop_reason::tool_use));
    p.replies.push_back(reply({block::make_text("Done.")}, stop_reason::end_turn));

    auto tools = builtin_tools();
    std::mutex mutex;
    std::condition_variable ready;
    int entered = 0;
    bool write_finished = false;
    bool alias_ran_in_order = false;
    bool overlapped = false;
    for (auto& t : tools) {
        if (!t.parallel_file_write) continue;
        t.run = [&, run = t.run](const json& input, tool_context& ctx) {
            const auto path = input.at("file_path").get<std::string>();
            if (path == "./A.TXT") {
                std::lock_guard lock(mutex);
                alias_ran_in_order = write_finished;
            } else {
                std::unique_lock lock(mutex);
                const bool first = ++entered == 1;
                ready.notify_all();
                if (first) overlapped = ready.wait_for(lock, std::chrono::seconds(1), [&] { return entered == 2; });
            }
            auto out = run(input, ctx);
            if (path == "b.txt") {
                std::lock_guard lock(mutex);
                write_finished = true;
            }
            return out;
        };
    }
    permission_rules rules;
    agent a(p, std::move(tools), rules, fs::canonical(dir.path));
    agent_events ev;
    cancel_token cancel;
    CHECK(a.run("edit and create", ev, cancel).stop == stop_reason::end_turn);
    CHECK(overlapped);
    CHECK(alias_ran_in_order);
    const auto& results = p.requests.at(1).messages.back().blocks;
    REQUIRE(results.size() == 4);
    CHECK(results[1].id == "edit");
    CHECK(results[2].id == "write");
    CHECK(results[3].id == "edit-again");
    for (const auto& result : results) CHECK_FALSE(result.is_error);
    CHECK(read_file(dir.path / "a.txt") == "last");
    CHECK(read_file(dir.path / "b.txt") == "new");
    CHECK(a.undo().size() == 2);
    CHECK(read_file(dir.path / "a.txt") == "old");
    CHECK_FALSE(fs::exists(dir.path / "b.txt"));
}

TEST_CASE("undo removes a file created and then edited through a Windows path alias") {
    temp_dir dir;
    scripted_provider p;
    p.replies.push_back(reply({
        block::make_tool_use("create", "Write", {{"file_path", "new.txt"}, {"content", "first"}}),
        block::make_tool_use("edit", "Edit", {{"file_path", "NEW.TXT"}, {"old_string", "first"}, {"new_string", "last"}}),
    }, stop_reason::tool_use));
    p.replies.push_back(reply({block::make_text("Done.")}, stop_reason::end_turn));
    permission_rules rules;
    agent a(p, builtin_tools(), rules, fs::canonical(dir.path));
    agent_events ev;
    cancel_token cancel;
    CHECK(a.run("create and edit", ev, cancel).stop == stop_reason::end_turn);
    CHECK(read_file(dir.path / "new.txt") == "last");
    CHECK(a.undo() == std::vector<std::string>{"new.txt"});
    CHECK_FALSE(fs::exists(dir.path / "new.txt"));
}

TEST_CASE("parallel write targets exclude Windows aliases and ambiguous paths") {
    temp_dir dir;
    const auto root = fs::canonical(dir.path);
    std::ofstream(root / "a.txt") << "old";
    const auto existing = parallel_write_target(root, root / "a.txt");
    const auto alias = parallel_write_target(root, root / "A.txt");
    REQUIRE(existing);
    REQUIRE(alias);
    CHECK_FALSE(independent_write_targets(*existing, *alias));
    const auto new_file = parallel_write_target(root, root / "new.txt");
    const auto new_alias = parallel_write_target(root, root / "NEW.txt");
    REQUIRE(new_file);
    REQUIRE(new_alias);
    CHECK_FALSE(independent_write_targets(*new_file, *new_alias));
    CHECK(independent_write_targets(*existing, *new_file));
    CHECK_FALSE(parallel_write_target(root, root / "missing-dir" / "a.txt"));
    CHECK_FALSE(parallel_write_target(root, root / "a.txt:stream"));
    std::error_code ec;
    fs::create_hard_link(root / "a.txt", root / "linked.txt", ec);
    if (!ec) {
        CHECK_FALSE(parallel_write_target(root, root / "a.txt"));
        CHECK_FALSE(parallel_write_target(root, root / "linked.txt"));
    }
}

TEST_CASE("undo retains failed files for a later retry") {
    temp_dir dir;
    std::ofstream(dir.path / "a.txt") << "original";
    scripted_provider p;
    p.replies.push_back(reply({block::make_tool_use("write", "Write", {
        {"file_path", "a.txt"}, {"content", "changed"}})}, stop_reason::tool_use));
    p.replies.push_back(reply({block::make_text("Done.")}, stop_reason::end_turn));
    permission_rules rules;
    agent a(p, builtin_tools(), rules, dir.path);
    cancel_token cancel;
    agent_events events;
    CHECK(a.run("change file", events, cancel).stop == stop_reason::end_turn);
    fs::remove(dir.path / "a.txt");
    fs::create_directory(dir.path / "a.txt");

    std::vector<std::string> failed;
    CHECK(a.undo(&failed).empty());
    CHECK(failed == std::vector<std::string>{"a.txt"});
    CHECK(a.can_undo());
    fs::remove(dir.path / "a.txt");
    std::ofstream(dir.path / "a.txt") << "still changed";
    failed.clear();
    CHECK(a.undo(&failed) == std::vector<std::string>{"a.txt"});
    CHECK(failed.empty());
    CHECK(read_file(dir.path / "a.txt") == "original");
    CHECK_FALSE(a.can_undo());
}

TEST_CASE("steering after a read prevents subsequent mutations and closes all tool calls") {
    temp_dir dir;
    std::ofstream(dir.path / "source.txt") << "source";
    scripted_provider p;
    p.replies.push_back(reply({
        block::make_tool_use("read", "Read", {{"file_path", "source.txt"}}),
        block::make_tool_use("write", "Write", {{"file_path", "new.txt"}, {"content", "new"}}),
        block::make_tool_use("glob", "Glob", {{"pattern", "*.txt"}}),
    }, stop_reason::tool_use));
    p.replies.push_back(reply({block::make_text("Done.")}, stop_reason::end_turn));
    permission_rules rules;
    rules.set_mode(permission_mode::yolo);
    agent a(p, builtin_tools(), rules, dir.path);
    agent_events ev;
    bool read_done = false;
    ev.tool_finished = [&](const tool& t, const json&, const tool_output&) { read_done = t.name == "Read"; };
    ev.take_steering = [&] {
        if (!std::exchange(read_done, false)) return std::vector<std::string>{};
        return std::vector<std::string>{"Do not write new.txt."};
    };
    cancel_token cancel;
    CHECK(a.run("read and write", ev, cancel).stop == stop_reason::end_turn);
    CHECK_FALSE(fs::exists(dir.path / "new.txt"));
    const auto& results = p.requests.at(1).messages.back().blocks;
    REQUIRE(results.size() == 4);
    CHECK(results[0].id == "read");
    CHECK_FALSE(results[0].is_error);
    CHECK(results[1].id == "write");
    CHECK(results[1].is_error);
    CHECK(results[2].id == "glob");
    CHECK(results[2].is_error);
    CHECK(results[3].text.find("Do not write new.txt.") != std::string::npos);
}

TEST_CASE("request timings report first output and distinguish compaction") {
    temp_dir dir;
    scripted_provider p;
    p.replies.push_back(reply({block::make_text("Done.")}, stop_reason::end_turn));
    p.replies.push_back(reply({block::make_text("Summary.")}, stop_reason::end_turn));
    p.replies.push_back(reply({}, stop_reason::error, false));
    permission_rules rules;
    agent a(p, {}, rules, dir.path);
    agent_events ev;
    std::vector<request_timing> timings;
    ev.request_finished = [&](const request_timing& timing) { timings.push_back(timing); };
    cancel_token cancel;
    CHECK(a.run("hello", ev, cancel).stop == stop_reason::end_turn);
    CHECK(a.compact(ev, cancel).stop == stop_reason::end_turn);
    CHECK(a.run("again", ev, cancel).stop == stop_reason::error);
    REQUIRE(timings.size() == 3);
    REQUIRE(timings[0].first_output);
    CHECK(*timings[0].first_output <= timings[0].elapsed);
    CHECK_FALSE(timings[0].compact);
    CHECK(timings[1].compact);
    CHECK_FALSE(timings[2].first_output.has_value());
}

TEST_CASE("incomplete reply dispatches nothing") {
    temp_dir dir;
    scripted_provider p;
    p.replies.push_back(reply({block::make_tool_use("1", "Write", {{"file_path", "x.txt"}, {"content", "x"}})},
                              stop_reason::error, false));
    permission_rules rules;
    rules.set_mode(permission_mode::yolo);
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
    std::vector<bool> timed;
    ev.tool_finished = [&](const tool&, const json&, const tool_output& out) { timed.push_back(out.elapsed.has_value()); };
    cancel_token cancel;
    a.run("build", ev, cancel);
    CHECK(asked == 2);
    CHECK(timed == std::vector<bool>{false, true});
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

TEST_CASE("hooks block tools before asking and can continue a finished reply") {
    temp_dir dir;
    scripted_provider p;
    p.replies.push_back(reply({block::make_tool_use("1", "Bash", {{"command", "echo hi"}})}, stop_reason::tool_use));
    p.replies.push_back(reply({block::make_text("done")}, stop_reason::end_turn));
    p.replies.push_back(reply({block::make_text("checked")}, stop_reason::end_turn));
    auto tools = builtin_tools();
    permission_rules rules;
    agent_hooks hooks;
    hooks.prompt = [](std::string_view, cancel_token&) {
        hook_verdict result;
        result.additional_context = "context from hook";
        return result;
    };
    hooks.pre_tool = [](const tool&, const json&, cancel_token&) { return hook_verdict{false, "blocked by fixture"}; };
    std::vector<bool> active;
    hooks.stop = [&](bool continued, cancel_token&) {
        active.push_back(continued);
        return continued ? hook_verdict{} : hook_verdict{false, "check again"};
    };
    agent a(p, std::move(tools), rules, dir.path, std::move(hooks));
    agent_events ev;
    ev.ask = [](const tool&, const json&) { FAIL("A hook already blocked the tool"); return answer::no; };
    cancel_token cancel;
    CHECK(a.run("go", ev, cancel).stop == stop_reason::end_turn);
    REQUIRE(p.requests.size() == 3);
    CHECK(p.requests[0].messages.back().blocks.back().text == "context from hook");
    CHECK(p.requests[1].messages.back().blocks.front().text == "blocked by fixture");
    CHECK(p.requests[2].messages.back().plain_text() == "check again");
    CHECK(active == std::vector<bool>{false, true});
}

TEST_CASE("auto-compaction checks the threshold after storing every tool result") {
    temp_dir dir;
    scripted_provider p;
    auto first = reply({block::make_tool_use("1", "Glob", {{"pattern", "*"}})}, stop_reason::tool_use);
    bool expect_compaction = true;
    first.tokens.input = 820;
    SUBCASE("At the threshold") {
        first.tokens.input = 800;
        expect_compaction = false;
    }
    p.replies.push_back(std::move(first));
    if (expect_compaction) {
        auto summary = reply({block::make_text("The user asked to list files.")}, stop_reason::end_turn);
        summary.tokens.output = 10;
        p.replies.push_back(std::move(summary));
    }
    p.replies.push_back(reply({block::make_text("Done.")}, stop_reason::end_turn));

    permission_rules rules;
    agent a(p, builtin_tools(), rules, dir.path);
    a.context_window = 1000;
    agent_events ev;
    int compacted = 0;
    ev.compacted = [&] { ++compacted; };
    cancel_token cancel;
    CHECK(a.run("List files", ev, cancel).stop == stop_reason::end_turn);
    REQUIRE(p.requests.size() == (expect_compaction ? 3 : 2));
    CHECK(compacted == (expect_compaction ? 1 : 0));
    const auto& second = p.requests[1];
    REQUIRE(second.messages.size() >= 3);
    CHECK(second.messages[2].blocks[0].type == block::kind::tool_result);
    CHECK(second.messages[2].blocks[0].id == "1");
    if (expect_compaction) {
        CHECK(second.tools.empty());
        CHECK(second.messages.back().plain_text().starts_with("Summarise"));
        CHECK(p.requests.back().messages.front().plain_text().starts_with("[Summary of the earlier conversation]"));
    } else {
        CHECK_FALSE(second.tools.empty());
    }
}

TEST_CASE("auto-compaction at the start keeps the new prompt and handles failure or cancellation") {
    temp_dir dir;
    scripted_provider p;
    auto summary = reply({block::make_text("Earlier summary.")}, stop_reason::end_turn);
    SUBCASE("Incomplete summary") { summary.complete = false; summary.stop = stop_reason::error; }
    SUBCASE("Cancelled summary") { summary.complete = false; summary.stop = stop_reason::cancelled; }
    const auto compact_stop = summary.stop;
    p.replies.push_back(std::move(summary));
    p.replies.push_back(reply({block::make_text("Done.")}, stop_reason::end_turn));
    permission_rules rules;
    agent a(p, builtin_tools(), rules, dir.path);
    a.context_window = 1000;
    a.context_tokens = 820;
    a.messages = {
        {message::role::user, {block::make_text("Earlier request")}},
        {message::role::assistant, {block::make_text("Earlier reply")}},
        {message::role::user, {block::make_text("Follow-up")}},
        {message::role::assistant, {block::make_text("Previous reply")}},
    };
    int noticed = 0;
    agent_events ev;
    ev.notice = [&](std::string_view) { ++noticed; };
    cancel_token cancel;
    const auto result = a.run("New prompt", ev, cancel);
    REQUIRE_FALSE(p.requests.empty());
    CHECK(p.requests[0].messages.size() == 5);
    CHECK(p.requests[0].messages.back().plain_text().starts_with("Summarise"));
    if (compact_stop == stop_reason::cancelled) {
        CHECK(result.stop == stop_reason::cancelled);
        CHECK(p.requests.size() == 1);
        CHECK(a.messages.size() == 4);
        CHECK(noticed == 0);
    } else {
        CHECK(result.stop == stop_reason::end_turn);
        REQUIRE(p.requests.size() == 2);
        CHECK(p.requests.back().messages.back().plain_text() == "New prompt");
        CHECK(p.requests.back().messages.size() == (compact_stop == stop_reason::end_turn ? 3 : 5));
        CHECK(noticed == (compact_stop == stop_reason::error ? 1 : 0));
    }
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
    r.set_mode(permission_mode::yolo);
    CHECK(r.check(bash, {{"command", "git push origin main"}}, root) == verdict::deny);
    CHECK(r.check(bash, {{"command", "rm -rf build"}}, root) == verdict::allow);
    r.set_mode(permission_mode::plan);
    CHECK(r.check(edit, {{"file_path", "x"}}, root) == verdict::deny);
    CHECK(r.check(read, {{"file_path", "x"}}, root) == verdict::allow);
    CHECK(command_segments("a 'b;c' | d && e") == std::vector<std::string>{"a 'b;c'", "d", "e"});
}

TEST_CASE("command grants apply only during their turn and keep explicit denials") {
    const auto tools = builtin_tools();
    const auto& bash = find(tools, "Bash");
    const auto root = fs::temp_directory_path();
    permission_rules rules;
    const json input{{"command", "fixture lint files"}};
    const std::vector<std::string> allowed{"Bash(fixture lint *)"};
    CHECK(rules.check(bash, input, root) == verdict::ask);
    CHECK(rules.check(bash, input, root, allowed) == verdict::allow);
    CHECK(rules.check(bash, input, root) == verdict::ask);
    rules.deny.push_back("Bash(fixture lint files)");
    CHECK(rules.check(bash, input, root, allowed) == verdict::deny);
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
