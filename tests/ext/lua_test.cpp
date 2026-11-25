#include "ext/lua/host.hpp"
#include "ext/hooks.hpp"

#include <doctest.h>

#include <fstream>
#include <random>

using namespace ode;
namespace fs = std::filesystem;

namespace {

struct lua_fixture {
    fs::path root = fs::temp_directory_path() / ("ode-lua-" + std::to_string(std::random_device{}()));
    lua_fixture() { fs::create_directory(root); }
    ~lua_fixture() { std::error_code ec; fs::remove_all(root, ec); }

    fs::path script(std::string_view name, std::string_view body) const {
        const auto file = root / name;
        std::ofstream(file, std::ios::binary) << body;
        return file;
    }
};

struct lua_test_provider final : provider {
    std::vector<model_request> requests;

    model_reply send(const model_request& req, const stream_events&, cancel_token&) override {
        requests.push_back(req);
        model_reply reply;
        reply.complete = true;
        if (requests.size() == 1) {
            reply.stop = stop_reason::tool_use;
            reply.content.blocks = {
                block::make_tool_use("a", "Bash", {{"command", "echo should-not-run"}}),
                block::make_tool_use("b", "LuaEdit", {{"file_path", "demo.txt"}})
            };
        } else {
            reply.stop = stop_reason::end_turn;
            reply.content.blocks = {block::make_text("done")};
        }
        return reply;
    }
};

} // namespace

TEST_CASE("Lua reload is lazy and creates a fresh restricted VM") {
    lua_fixture files;
    lua_host host;
    cancel_token cancel;
    CHECK(host.reload({files.root / "missing.lua"}, cancel).empty());
    CHECK_FALSE(host.active());
    const auto path = files.script("init.lua", R"(
        assert(stale == nil)
        assert(ode.api_version == 1)
        assert(io == nil and os == nil and debug == nil and package == nil)
        assert(loadfile == nil and dofile == nil)
        assert(string.upper('lua') == 'LUA')
        stale = true
    )");
    CHECK(host.reload({path}, cancel).empty());
    CHECK(host.active());
    CHECK(host.reload({path}, cancel).empty());
    CHECK(host.reload({}, cancel).empty());
    CHECK_FALSE(host.active());
}

TEST_CASE("Lua terminates an infinite script and enforces the heap limit") {
    lua_fixture files;
    lua_host host;
    cancel_token cancel;
    const auto loop = files.script("loop.lua", "while true do end");
    const auto memory = files.script("memory.lua", "local _ = string.rep('x', 16 * 1024 * 1024)");
    const auto valid = files.script("valid.lua", "assert(ode.api_version == 1)");
    const auto warnings = host.reload({loop, memory, valid}, cancel);
    REQUIRE(warnings.size() == 2);
    CHECK(warnings[0].find("execution limit") != std::string::npos);
    CHECK(warnings[1].find("memory") != std::string::npos);
}

TEST_CASE("Lua hooks and tools use the agent tool registry, permissions and undo") {
    lua_fixture files;
    lua_host host;
    cancel_token cancel;
    files.script("demo.txt", "before");
    const auto path = files.script("init.lua", R"(
        ode.on('pre_tool', function(ctx)
            if ctx.tool_name == 'Bash' then
                return {hookSpecificOutput = {
                    permissionDecision = 'deny', permissionDecisionReason = 'blocked in Lua'}}
            end
        end)
        ode.tool{
            name = 'LuaEdit', description = 'Edit a file', access = 'execute',
            schema = {type = 'object', properties = {file_path = {type = 'string'}}, required = {'file_path'}},
            run = function(input)
                assert(ode.exec('echo should-not-run').text == 'blocked in Lua')
                assert(not ode.fs.read(input.file_path).is_error)
                assert(ode.fs.edit(input.file_path, 'before', 'after').is_error)
                assert(not ode.fs.read(input.file_path).is_error)
                return ode.fs.edit(input.file_path, 'before external', 'after')
            end
        }
        ode.command('check', function(path)
            return ode.fs.write(path, 'changed')
        end)
    )");
    REQUIRE(host.reload({path}, cancel).empty());
    const auto bad = files.script("bad.lua", "ode.command('rollback', function() end); error('init failed')");
    REQUIRE(host.reload({path, bad}, cancel).size() == 1);
    CHECK(host.commands() == std::vector<std::string>{"check"});

    auto tools = builtin_tools();
    auto added = host.tools();
    REQUIRE(added.size() == 1);
    CHECK(added.front().schema["required"] == json::array({"file_path"}));
    tools.push_back(std::move(added.front()));
    auto hooks = make_agent_hooks({}, [&] { return hook_session{"local", {}, files.root}; },
        [&](hook_type type, const json& input, cancel_token& c) {
            return host.run_hooks(type, input, c, parse_hook_output);
        });
    lua_test_provider model;
    permission_rules rules;
    agent core(model, std::move(tools), rules, files.root, std::move(hooks));
    agent_events events;
    std::vector<std::string> asks;
    events.ask = [&](const tool& t, const json&) { asks.push_back(t.name); return answer::yes; };
    bool modified = false;
    events.tool_finished = [&](const tool& t, const json&, const tool_output& out) {
        if (!modified && t.name == "Read" && !out.is_error) {
            files.script("demo.txt", "before external");
            modified = true;
        }
    };

    CHECK(core.run("test", events, cancel).stop == stop_reason::end_turn);
    REQUIRE(model.requests.size() == 2);
    const auto& results = model.requests.back().messages.back().blocks;
    REQUIRE(results.size() == 2);
    CHECK(results[0].is_error);
    CHECK(results[0].text == "blocked in Lua");
    CHECK_MESSAGE(!results[1].is_error, results[1].text);
    CHECK(asks == std::vector<std::string>{"LuaEdit"});
    CHECK(read_file(files.root / "demo.txt") == std::optional<std::string>{"after"});
    CHECK(core.undo() == std::vector<std::string>{"demo.txt"});
    CHECK(read_file(files.root / "demo.txt") == std::optional<std::string>{"before external"});

    rules.set_mode(permission_mode::plan);
    const auto command = [&](tool_context& ctx) { return host.run_command("check", "new.txt", ctx); };
    CHECK(core.run_local(command, events, cancel).is_error);
    CHECK_FALSE(fs::exists(files.root / "new.txt"));
    rules.set_mode(permission_mode::normal);
    CHECK_FALSE(core.run_local(command, events, cancel).is_error);
    CHECK(read_file(files.root / "new.txt") == std::optional<std::string>{"changed"});
    CHECK(core.undo() == std::vector<std::string>{"new.txt"});
    CHECK_FALSE(fs::exists(files.root / "new.txt"));
}
