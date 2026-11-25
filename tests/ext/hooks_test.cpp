#include "base/utf.hpp"
#include "ext/hooks.hpp"

#include <doctest.h>

#include <cstdlib>
#include <fstream>
#include <random>

using namespace ode;

namespace {

struct hook_workspace {
    fs::path path = fs::temp_directory_path() / ("ode hooks-" + std::to_string(std::random_device{}()));
    std::string home = narrow(user_home().wstring());

    hook_workspace() {
        fs::create_directories(path / ".claude");
        fs::create_directories(path / ".ode");
        fs::create_directories(path / "home");
        _putenv_s("USERPROFILE", (path / "home").string().c_str());
    }
    ~hook_workspace() {
        _putenv_s("USERPROFILE", home.c_str());
        std::error_code error;
        fs::remove_all(path, error);
    }
};

} // namespace

TEST_CASE("hooks merge across scopes and parse command matchers and timeouts") {
    hook_workspace ws;
    std::ofstream(ws.path / ".claude" / "settings.json") << R"({"hooks":{"PreToolUse":[
      {"matcher":"Bash|Edit","hooks":[{"type":"command","command":"first","timeout":30},{"type":"http","url":"unused"}]}
    ]}})";
    std::ofstream(ws.path / ".ode" / "settings.json") << R"({"hooks":{"PreToolUse":[
      {"matcher":"[literal","hooks":[{"type":"command","command":"echo {\"decision\":\"block\",\"reason\":\"literal\"}"}]}
    ]}})";

    const auto settings = load_settings(ws.path);
    REQUIRE(settings.hooks.size() == 2);
    const auto& first = settings.hooks[0];
    CHECK(first.command == "first");
    CHECK(first.timeout.count() == 30);
    REQUIRE(first.matcher_regex);
    CHECK(std::regex_match("Bash", *first.matcher_regex));
    CHECK_FALSE(std::regex_match("Read", *first.matcher_regex));
    CHECK(settings.hooks[1].timeout.count() == 60);
    CHECK_FALSE(settings.hooks[1].matcher_regex);
    REQUIRE(settings.warnings.size() == 1);
    CHECK(settings.warnings.front().find("[literal") != std::string::npos);

    hook_context ctx{};
    ctx.type = hook_type::pre_tool_use;
    ctx.session.cwd = ws.path;
    ctx.tool_name = "Read";
    cancel_token cancel;
    CHECK(execute_hook(settings.hooks[1], ctx, cancel, std::nullopt).allow);
    ctx.tool_name = "[literal";
    const auto result = execute_hook(settings.hooks[1], ctx, cancel, std::nullopt);
    CHECK_FALSE(result.allow);
    CHECK(result.message == "literal");
}

TEST_CASE("exit code 2 reports only stderr and hook stdin uses Claude Code fields") {
    hook_workspace ws;
    hook_config hook{};
    hook.type = hook_type::pre_tool_use;
    const auto script = ws.path / "hook-fixture.cmd";
    hook.command = '"' + narrow(script.wstring()) + '"';
    std::ofstream(script) << "@echo off\nsetlocal EnableDelayedExpansion\n"
        "set /p payload=\necho ignored\necho(!payload! 1>&2\nexit /b 2\n";

    hook_context ctx{};
    ctx.type = hook.type;
    ctx.session = {"session", ws.path / "transcript.jsonl", ws.path};
    ctx.tool_name = "Bash";
    ctx.tool_input = {{"command", "echo hi"}};
    cancel_token cancel;
    const auto result = execute_hook(hook, ctx, cancel, std::nullopt);
    CHECK_FALSE(result.allow);
    INFO(result.message);
    const auto input = json::parse(result.message, nullptr, false);
    REQUIRE(input.is_object());
    CHECK(input["session_id"] == "session");
    CHECK(input["transcript_path"] == narrow(ctx.session.transcript.wstring()));
    CHECK(input["cwd"] == narrow(ws.path.wstring()));
    CHECK(input["hook_event_name"] == "PreToolUse");
    CHECK(input["tool_name"] == "Bash");
    CHECK(input["tool_input"] == ctx.tool_input);
}

TEST_CASE("hook JSON decisions and the workspace environment reach command hooks") {
    hook_workspace ws;
    hook_context ctx{};
    ctx.type = hook_type::pre_tool_use;
    ctx.session.cwd = ws.path;
    ctx.tool_name = "Bash";
    hook_config hook{};
    hook.type = ctx.type;
    hook.command = "if /i \"%CLAUDE_PROJECT_DIR%\"==\"%CD%\" echo "
        R"({"hookSpecificOutput":{"permissionDecision":"deny","permissionDecisionReason":"denied"}})";
    cancel_token cancel;
    const auto result = execute_hook(hook, ctx, cancel, std::nullopt);
    CHECK_FALSE(result.allow);
    CHECK(result.message == "denied");
}
