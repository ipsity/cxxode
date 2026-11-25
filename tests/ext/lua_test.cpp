#include "ext/lua/host.hpp"

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
