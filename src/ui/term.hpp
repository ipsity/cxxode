#pragma once

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ode::ui {

enum class key_kind {
    character, enter, newline, backspace, del, left, right, up, down, home, end,
    page_up, page_down, tab, shift_tab, escape, ctrl, word_left, word_right,
};

struct key {
    key_kind kind = key_kind::character;
    std::string text;  // character: UTF-8 text
    char letter = 0;   // ctrl: lower-case letter
};

struct size {
    int columns = 80;
    int rows = 24;
    bool operator==(const size&) const = default;
};

// Raw-mode console for the interactive UI. Restores the original modes on destruction.
class terminal {
public:
    terminal();
    ~terminal();
    terminal(const terminal&) = delete;
    terminal& operator=(const terminal&) = delete;

    size dimensions() const;
    // Waits up to wait_ms for input. Returns nullopt when input is closed.
    std::optional<std::vector<key>> read(int wait_ms);
    void write(std::string_view utf8) const;

    // True when both stdin and stdout are interactive consoles.
    static bool interactive();

private:
    struct native;
    std::unique_ptr<native> native_;
};

// Writes UTF-8 to stdout or stderr, using the console API when attached to a console.
void write_stream(bool error_stream, std::string_view utf8);
bool stdin_is_terminal();

} // namespace ode::ui
