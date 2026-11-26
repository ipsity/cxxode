#include "tools/tool.hpp"

#include <doctest.h>

#include <chrono>
#include <fstream>
#include <random>

using namespace ode;

namespace {

struct temp_dir {
    fs::path path;
    temp_dir() {
        std::random_device rd;
        path = fs::temp_directory_path() / ("ode-test-" + std::to_string(rd()) + std::to_string(rd()));
        fs::create_directories(path);
    }
    ~temp_dir() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
    void put(const std::string& rel, const std::string& content) const {
        fs::create_directories((path / rel).parent_path());
        std::ofstream(path / rel, std::ios::binary) << content;
    }
    std::string get(const std::string& rel) const { return read_file(path / rel).value_or("<missing>"); }
};

const tool& find(const std::vector<tool>& tools, std::string_view name) {
    for (const auto& t : tools) if (t.name == name) return t;
    throw std::runtime_error("no tool");
}

struct fixture {
    temp_dir dir;
    std::vector<tool> tools = builtin_tools();
    cancel_token cancel;
    turn_backup backup;
    std::map<fs::path, file_stamp> seen;
    tool_context ctx{dir.path, &cancel, &backup, &seen, {}};
    tool_output run(std::string_view name, const json& input) { return find(tools, name).run(input, ctx); }
};

} // namespace

TEST_CASE("glob matching") {
    CHECK(glob_match("*.cpp", "a.cpp"));
    CHECK_FALSE(glob_match("*.cpp", "src/a.cpp"));
    CHECK(glob_match("**/*.cpp", "a.cpp"));
    CHECK(glob_match("**/*.cpp", "src/x/a.cpp"));
    CHECK(glob_match("src/**", "src/x/a.cpp"));
    CHECK(glob_match("*.{h,hpp}", "a.hpp"));
    CHECK_FALSE(glob_match("*.{h,hpp}", "a.cpp"));
    CHECK(glob_match("a?c", "abc"));
}

TEST_CASE("read numbers lines and paginates") {
    fixture f;
    f.dir.put("a.txt", "one\r\ntwo\r\nthree\r\n");
    auto r = f.run("Read", {{"file_path", "a.txt"}});
    CHECK_FALSE(r.is_error);
    CHECK(r.text == "     1\tone\n     2\ttwo\n     3\tthree\n");
    r = f.run("Read", {{"file_path", "a.txt"}, {"offset", 2}, {"limit", 1}});
    CHECK(r.text.starts_with("     2\ttwo\n"));
    CHECK(r.text.find("offset") != std::string::npos);
    CHECK(f.run("Read", {{"file_path", "missing.txt"}}).is_error);
}

TEST_CASE("edit preserves CRLF, requires uniqueness and backs up") {
    fixture f;
    f.dir.put("c.txt", "a = 1\r\nb = 1\r\n");
    f.run("Read", {{"file_path", "c.txt"}});
    CHECK(f.run("Edit", {{"file_path", "c.txt"}, {"old_string", "= 1"}, {"new_string", "= 2"}}).is_error);
    auto r = f.run("Edit", {{"file_path", "c.txt"}, {"old_string", "a = 1\nb"}, {"new_string", "a = 2\nb"}});
    CHECK_FALSE(r.is_error);
    CHECK(f.dir.get("c.txt") == "a = 2\r\nb = 1\r\n");
    CHECK(r.summary == "+2 -2");
    CHECK(r.detail == "- a = 1\n- b\n+ a = 2\n+ b\n");
    r = f.run("Edit", {{"file_path", "c.txt"}, {"old_string", " = "}, {"new_string", ": "}, {"replace_all", true}});
    CHECK(f.dir.get("c.txt") == "a: 2\r\nb: 1\r\n");

    r = f.run("Write", {{"file_path", "new/d.txt"}, {"content", "x"}});
    CHECK(f.dir.get("new/d.txt") == "x");
    CHECK(r.detail == "+ x\n");
    restore(f.backup, f.dir.path);
    CHECK(f.dir.get("c.txt") == "a = 1\r\nb = 1\r\n");
    CHECK_FALSE(fs::exists(f.dir.path / "new/d.txt"));
}

TEST_CASE("overwrite preview includes additions after many removed lines") {
    fixture f;
    f.dir.put("m.txt", "header\r\n1\r\n2\r\n3\r\n4\r\n5\r\n6\r\n7\r\n8\r\n9\r\nfooter\r\n");
    auto r = f.run("Write", {{"file_path", "m.txt"}, {"content", "header\r\nnew\r\nfooter\r\n"}});
    REQUIRE_FALSE(r.is_error);
    CHECK(r.detail.starts_with("- 1\n"));
    CHECK(r.detail.ends_with("… -1 more lines\n+ new\n"));
    CHECK(r.detail.find("header") == std::string::npos);
    CHECK(r.detail.find("footer") == std::string::npos);
}

TEST_CASE("edit refuses a file changed since it was read") {
    fixture f;
    f.dir.put("e.txt", "hello");
    f.run("Read", {{"file_path", "e.txt"}});
    f.dir.put("e.txt", "hello world");
    fs::last_write_time(f.dir.path / "e.txt", fs::file_time_type::clock::now() + std::chrono::seconds(5));
    auto r = f.run("Edit", {{"file_path", "e.txt"}, {"old_string", "hello"}, {"new_string", "bye"}});
    CHECK(r.is_error);
    CHECK(r.text.find("changed on disk") != std::string::npos);
}

TEST_CASE("grep and glob outside git") {
    fixture f;
    f.dir.put("src/a.cpp", "int main() {}\n// TODO fix\n");
    f.dir.put("src/b.hpp", "// todo later\n");
    f.dir.put("node_modules/x.cpp", "TODO\n");
    auto r = f.run("Grep", {{"pattern", "TODO"}});
    CHECK(r.text == "src/a.cpp\n");
    r = f.run("Grep", {{"pattern", "todo"}, {"-i", true}, {"output_mode", "content"}, {"glob", "*.hpp"}});
    CHECK(r.text == "src/b.hpp:1:// todo later\n");
    r = f.run("Glob", {{"pattern", "**/*.cpp"}});
    CHECK(r.text == "src/a.cpp\n");
}

TEST_CASE("bash runs a command and reports exit codes") {
    fixture f;
    auto r = f.run("Bash", {{"command", "echo hi"}});
    CHECK_FALSE(r.is_error);
    CHECK(r.text.find("hi") != std::string::npos);
    r = f.run("Bash", {{"command", "exit 3"}});
    CHECK(r.is_error);
    CHECK(r.text.find("exit code 3") != std::string::npos);
}

TEST_CASE("bash honours cancellation") {
    fixture f;
    f.cancel.cancel();
    const auto began = std::chrono::steady_clock::now();
    auto r = f.run("Bash", {{"command", "sleep 20"}});
    CHECK(r.is_error);
    CHECK(std::chrono::steady_clock::now() - began < std::chrono::seconds(5));
}
