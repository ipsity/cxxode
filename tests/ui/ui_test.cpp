#include "app/command_history.hpp"
#include "ui/editor.hpp"
#include "ui/render.hpp"
#include "ui/text.hpp"

#include <doctest.h>

#include <random>

using namespace ode;

TEST_CASE("saving shared command history keeps entries from other workspaces") {
    const auto dir = fs::temp_directory_path() / ("ode-history-" + std::to_string(std::random_device{}()));
    fs::create_directories(dir);
    const auto path = dir / "command_history.txt";
    command_history first, second, merged;
    first.add(R"(read C:\work)");
    first.save(path);
    second.load(path);
    first.add("first workspace");
    second.add("second\nworkspace");
    first.save(path);
    second.save(path);
    merged.load(path);
    CHECK(merged.previous() == "second\nworkspace");
    CHECK(merged.previous() == "first workspace");
    CHECK(merged.previous() == R"(read C:\work)");
    fs::remove_all(dir);
}

TEST_CASE("display width counts CJK and emoji as two columns") {
    CHECK(text::width("abc") == 3);
    CHECK(text::width("空配置") == 6);
    CHECK(text::width("👍🏽") == 2);
    CHECK(text::cells("e\xcc\x81x").size() == 2);  // e + combining acute is one cell
}

TEST_CASE("truncation keeps whole graphemes") {
    CHECK(text::keep_tail("abcdef", 4) == "…def");
    CHECK(text::keep_head("中文测试", 5) == "中文…");
    CHECK(text::keep_head("short", 10) == "short");
}

TEST_CASE("layout wraps between words and inside CJK runs") {
    ui::color_enabled() = false;
    auto rows = ui::layout({{"hello world again", ui::tone::normal}}, 11, {"", 0}, {"", 0});
    REQUIRE(rows.size() == 2);
    CHECK(rows[0] == "hello world");
    CHECK(rows[1] == "again");
    rows = ui::layout({{"一二三四五", ui::tone::normal}}, 6, {"", 0}, {"", 0});
    REQUIRE(rows.size() == 2);
    CHECK(rows[0] == "一二三");
    CHECK(rows[1] == "四五");
    ui::color_enabled() = true;
}

TEST_CASE("markdown lines") {
    ui::color_enabled() = false;
    ui::markdown_state s;
    CHECK(ui::markdown_line("- item", s, 40) == std::vector<std::string>{"  • item"});
    CHECK(ui::markdown_line("```cpp", s, 40).empty());
    CHECK(ui::markdown_line("int x;", s, 40) == std::vector<std::string>{"  │ int x;"});
    CHECK(ui::markdown_line("```", s, 40).empty());
    CHECK(ui::markdown_line("**bold** text", s, 40) == std::vector<std::string>{"  bold text"});
    ui::color_enabled() = true;
}

TEST_CASE("editor moves by grapheme and wraps") {
    ui::editor e;
    e.insert("你好");
    e.backspace();
    CHECK(e.text() == "你");
    e.insert("a\nbc");
    e.home();
    CHECK(e.cursor() == e.text().find('\n') + 1);
    CHECK(e.vertical(-1, 20));
    CHECK_FALSE(e.vertical(-1, 20));
    e.set("abcdefgh");
    const auto l = e.arrange(4);
    CHECK(l.rows.size() == 3);  // two full rows plus the cursor row at the end
    CHECK(l.cursor_row == 2);
    CHECK(l.cursor_column == 0);
}

TEST_CASE("editor history keeps the unsent draft") {
    ui::editor e;
    e.remember("first");
    e.remember("second");
    e.set("draft");
    e.history_prev();
    CHECK(e.text() == "second");
    e.history_prev();
    CHECK(e.text() == "first");
    e.history_next();
    e.history_next();
    CHECK(e.text() == "draft");
}
