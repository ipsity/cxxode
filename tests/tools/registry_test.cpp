#include "tools/tool.hpp"

#include <doctest.h>

using namespace ode;

namespace {

tool sample_tool(std::string name, std::string description, tool_exposure exposure) {
    tool result;
    result.name = std::move(name);
    result.description = std::move(description);
    result.schema = {{"type", "object"}};
    result.access = tool_access::read;
    result.exposure = exposure;
    result.run = [](const json&, tool_context&) { return tool_output{}; };
    return result;
}

} // namespace

TEST_CASE("tool registry declares direct tools and activates deferred matches") {
    tool_registry registry({
        sample_tool("Read", "Read a workspace file", tool_exposure::direct),
        sample_tool("mcp__docs__search", "Search documentation", tool_exposure::deferred),
        sample_tool("mcp__db__query", "Run a database query", tool_exposure::deferred),
    });

    auto declared = registry.model_tools();
    REQUIRE(declared.size() == 1);
    CHECK(declared.front().name == "Read");

    const auto matches = registry.search_and_activate("documentation search", 5);
    REQUIRE(matches.size() == 1);
    CHECK(matches.front().name == "mcp__docs__search");
    declared = registry.model_tools();
    REQUIRE(declared.size() == 2);
    CHECK(declared.back().name == "mcp__docs__search");
}

TEST_CASE("tool search registers matches for the next model request") {
    auto search = make_tool_search_tool();
    tool_context context;
    context.search_tools = [](std::string_view query, std::size_t limit) {
        CHECK(query == "documentation search");
        CHECK(limit == 3);
        return std::vector<tool_search_result>{{"mcp__docs__search", "Search documentation"}};
    };

    const auto result = search.run({{"query", "documentation search"}, {"limit", 3}}, context);
    CHECK_FALSE(result.is_error);
    CHECK(result.text.find("mcp__docs__search") != std::string::npos);
}
