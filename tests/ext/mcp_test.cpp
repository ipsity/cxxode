#include "config/config.hpp"
#include "ext/mcp.hpp"

#include <doctest.h>
#include <fstream>
#include <windows.h>

using namespace ode;

namespace {

// The test binary and ode.exe are both written to the build directory.
fs::path ode_executable() {
    wchar_t buffer[32768];
    const auto n = GetModuleFileNameW(nullptr, buffer, static_cast<DWORD>(std::size(buffer)));
    REQUIRE(n > 0);
    return fs::path(buffer).parent_path() / "ode.exe";
}

fs::path scratch() {
    const auto dir = fs::temp_directory_path() / "ode-mcp-test";
    fs::remove_all(dir);
    fs::create_directories(dir);
    return dir;
}

} // namespace

TEST_CASE(".mcp.json merges with settings and the later scope wins") {
    const auto dir = scratch();
    fs::create_directories(dir / ".claude");
    {
        std::ofstream f(dir / ".claude" / "settings.json");
        f << R"({"mcpServers":{"old":{"command":"from-settings"},"kept":{"command":"kept"}}})";
    }
    {
        std::ofstream f(dir / ".mcp.json");
        f << R"({"new":{"command":"from-mcp"},"old":{"command":"overridden"}})";
    }

    const auto settings = load_settings(dir);
    CHECK(settings.mcp_servers["old"]["command"] == "overridden");
    CHECK(settings.mcp_servers["kept"]["command"] == "kept");
    CHECK(settings.mcp_servers["new"]["command"] == "from-mcp");
    fs::remove_all(dir);
}

TEST_CASE("newline JSON-RPC handshake lists and calls a tool") {
    mcp_client::server_config cfg;
    cfg.name = "fixture";
    cfg.command = ode_executable().string();
    cfg.args = {"--mcp-test-server"};

    auto client = std::make_shared<mcp_client>(cfg);
    const bool ok = client->initialize(std::chrono::seconds(5));
    INFO("command: ", cfg.command);
    INFO("initialize error: ", client->error());
    REQUIRE(ok);

    const auto tools = make_mcp_tools({client});
    REQUIRE(tools.size() == 1);
    CHECK(tools[0].name == "mcp__fixture__echo");
    CHECK(tools[0].exposure == tool_exposure::deferred);
    auto validated = tools[0];
    validated.schema = {{"type", "object"}, {"properties", {{"text", {{"type", "string"}}}}},
                        {"required", {"text"}}, {"additionalProperties", false}};
    CHECK(tool_input_error(validated, {{"text", 42}}).find("text must match type") != std::string::npos);
    CHECK(tool_input_error(validated, json::object()).find("missing required field 'text'") != std::string::npos);
    CHECK(tool_input_error(validated, {{"text", "pong"}, {"extra", true}}).find("does not allow field 'extra'") != std::string::npos);

    tool_context ctx;
    const auto output = tools[0].run({{"text", "pong"}}, ctx);
    CHECK_FALSE(output.is_error);
    CHECK(output.text.find("pong") != std::string::npos);

    cfg.name = "direct_fixture";
    cfg.exposure = tool_exposure::direct;
    auto direct_client = std::make_shared<mcp_client>(cfg);
    REQUIRE(direct_client->initialize(std::chrono::seconds(5)));
    const auto direct_tools = make_mcp_tools({direct_client});
    REQUIRE(direct_tools.size() == 1);
    CHECK(direct_tools[0].exposure == tool_exposure::direct);
}
