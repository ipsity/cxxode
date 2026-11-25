#include "ui/term.hpp"
#include "base/utf.hpp"

#include <io.h>

#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace ode::ui {

struct terminal::native {
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD original_in = 0;
    DWORD original_out = 0;
    UINT original_cp = 0;
    wchar_t high_surrogate = 0;
    int wheel_delta = 0;
};

bool terminal::interactive() {
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) && GetConsoleMode(GetStdHandle(STD_OUTPUT_HANDLE), &mode);
}

bool stdin_is_terminal() {
    DWORD mode = 0;
    return GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &mode) != 0;
}

terminal::terminal() : native_(std::make_unique<native>()) {
    auto& n = *native_;
    if (!GetConsoleMode(n.in, &n.original_in) || !GetConsoleMode(n.out, &n.original_out)) {
        throw std::runtime_error("The interactive UI needs a console. Use ode -p \"...\" for scripts.");
    }
    n.original_cp = GetConsoleOutputCP();
    // Raw key events: Ctrl+C arrives as a key, and modifiers on Tab and Enter stay visible.
    const DWORD input = (n.original_in | ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT | ENABLE_MOUSE_INPUT) &
        ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_QUICK_EDIT_MODE |
          ENABLE_VIRTUAL_TERMINAL_INPUT);
    const DWORD output = n.original_out | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    SetConsoleMode(n.in, input);
    SetConsoleMode(n.out, output);
    SetConsoleOutputCP(CP_UTF8);
}

terminal::~terminal() {
    auto& n = *native_;
    write("\x1b[0m\x1b[?25h");
    SetConsoleMode(n.in, n.original_in);
    SetConsoleMode(n.out, n.original_out);
    if (n.original_cp) SetConsoleOutputCP(n.original_cp);
}

size terminal::dimensions() const {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (GetConsoleScreenBufferInfo(native_->out, &info)) {
        return {info.srWindow.Right - info.srWindow.Left + 1, info.srWindow.Bottom - info.srWindow.Top + 1};
    }
    return {};
}

std::vector<std::string> terminal::visible_lines() const {
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (!GetConsoleScreenBufferInfo(native_->out, &info)) return {};

    std::vector<std::string> lines;
    const auto window = info.srWindow;
    const auto columns = static_cast<SHORT>(window.Right - window.Left + 1);
    std::vector<CHAR_INFO> cells(columns);
    const auto last = std::clamp(info.dwCursorPosition.Y, window.Top, window.Bottom);
    for (int y = window.Top; y <= last; ++y) {
        const auto row = static_cast<SHORT>(y);
        SMALL_RECT region{window.Left, row, window.Right, row};
        if (!ReadConsoleOutputW(native_->out, cells.data(), {columns, 1}, {0, 0}, &region)) return {};
        std::wstring line;
        for (const auto& cell : cells) {
            if (!(cell.Attributes & COMMON_LVB_TRAILING_BYTE)) {
                const auto ch = cell.Char.UnicodeChar;
                line.push_back(ch >= L' ' ? ch : L' ');
            }
        }
        const auto end = line.find_last_not_of(L' ');
        line.resize(end == std::wstring::npos ? 0 : end + 1);
        lines.push_back(narrow(line));
    }
    while (!lines.empty() && lines.back().empty()) lines.pop_back();
    const auto first = std::ranges::find_if(lines, [](const auto& line) { return !line.empty(); });
    lines.erase(lines.begin(), first);
    return lines;
}

std::optional<std::vector<key>> terminal::read(int wait_ms) {
    auto& n = *native_;
    std::vector<key> keys;
    const auto wait = WaitForSingleObject(n.in, static_cast<DWORD>(wait_ms));
    if (wait == WAIT_TIMEOUT) return keys;
    if (wait != WAIT_OBJECT_0) return std::nullopt;
    INPUT_RECORD batch[256];
    DWORD count = 0;
    if (!ReadConsoleInputW(n.in, batch, static_cast<DWORD>(std::size(batch)), &count)) return std::nullopt;

    for (DWORD i = 0; i < count; ++i) {
        if (batch[i].EventType == MOUSE_EVENT) {
            const auto& mouse = batch[i].Event.MouseEvent;
            if (mouse.dwEventFlags == MOUSE_WHEELED) {
                n.wheel_delta += static_cast<SHORT>(HIWORD(mouse.dwButtonState));
                const int steps = n.wheel_delta / WHEEL_DELTA;
                n.wheel_delta %= WHEEL_DELTA;
                if (steps) keys.push_back({key_kind::scroll, {}, 0, -3 * steps});
            }
            continue;
        }
        if (batch[i].EventType != KEY_EVENT || !batch[i].Event.KeyEvent.bKeyDown) continue;
        const auto& k = batch[i].Event.KeyEvent;
        const auto state = k.dwControlKeyState;
        const bool ctrl = state & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED);
        const bool alt = state & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED);
        const bool shift = state & SHIFT_PRESSED;
        key out;
        bool known = true;
        switch (k.wVirtualKeyCode) {
            case VK_RETURN: {
                // An Enter followed by more input in the same burst is part of a paste.
                bool more = false;
                for (DWORD j = i + 1; j < count && !more; ++j)
                    more = batch[j].EventType == KEY_EVENT && batch[j].Event.KeyEvent.bKeyDown;
                if (!more) {
                    // Mouse movement and resize events can be queued alongside a single Enter.
                    INPUT_RECORD queued[256];
                    DWORD pending = 0;
                    if (PeekConsoleInputW(n.in, queued, static_cast<DWORD>(std::size(queued)), &pending)) {
                        more = std::any_of(queued, queued + pending, [](const INPUT_RECORD& event) {
                            return event.EventType == KEY_EVENT && event.Event.KeyEvent.bKeyDown;
                        });
                    }
                }
                out.kind = shift || alt || ctrl || more ? key_kind::newline : key_kind::enter;
                break;
            }
            case VK_BACK: out.kind = key_kind::backspace; break;
            case VK_DELETE: out.kind = key_kind::del; break;
            case VK_LEFT: out.kind = ctrl ? key_kind::word_left : key_kind::left; break;
            case VK_RIGHT: out.kind = ctrl ? key_kind::word_right : key_kind::right; break;
            case VK_UP: out.kind = key_kind::up; break;
            case VK_DOWN: out.kind = key_kind::down; break;
            case VK_HOME: out.kind = key_kind::home; break;
            case VK_END: out.kind = key_kind::end; break;
            case VK_PRIOR: out.kind = key_kind::page_up; break;
            case VK_NEXT: out.kind = key_kind::page_down; break;
            case VK_TAB: out.kind = shift ? key_kind::shift_tab : key_kind::tab; break;
            case VK_ESCAPE: out.kind = key_kind::escape; break;
            default: known = false;
        }
        if (!known && ctrl && !alt && k.wVirtualKeyCode >= 'A' && k.wVirtualKeyCode <= 'Z') {
            out.kind = k.wVirtualKeyCode == 'J' ? key_kind::newline : key_kind::ctrl;
            out.letter = static_cast<char>('a' + (k.wVirtualKeyCode - 'A'));
            known = true;
        }
        if (!known && !ctrl && !alt && !shift && k.wVirtualKeyCode >= VK_F1 && k.wVirtualKeyCode <= VK_F12) {
            out.kind = key_kind::function_key;
            out.number = k.wVirtualKeyCode - VK_F1 + 1;
            known = true;
        }
        if (!known) {
            const wchar_t unit = k.uChar.UnicodeChar;
            if (!unit) continue;
            if (unit >= 0xd800 && unit <= 0xdbff) {
                n.high_surrogate = unit;
                continue;
            }
            if (n.high_surrogate && unit >= 0xdc00 && unit <= 0xdfff) {
                const wchar_t pair[]{n.high_surrogate, unit};
                out.text = narrow({pair, 2});
            } else if (unit == L'\t') {
                out.text = "    ";
            } else if (unit >= 0x20) {
                out.text = narrow({&unit, 1});
            } else {
                n.high_surrogate = 0;
                continue;
            }
            n.high_surrogate = 0;
        }
        const int repeat = std::clamp<int>(k.wRepeatCount, 1, 64);
        for (int r = 0; r < repeat; ++r) keys.push_back(out);
    }
    return keys;
}

void terminal::write(std::string_view utf8) const {
    if (utf8.empty()) return;
    const auto wide = widen(utf8);
    DWORD written = 0;
    WriteConsoleW(native_->out, wide.data(), static_cast<DWORD>(wide.size()), &written, nullptr);
}

void write_stream(bool error_stream, std::string_view utf8) {
    if (utf8.empty()) return;
    const HANDLE h = GetStdHandle(error_stream ? STD_ERROR_HANDLE : STD_OUTPUT_HANDLE);
    DWORD mode = 0;
    if (GetConsoleMode(h, &mode)) {
        SetConsoleMode(h, mode | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
        const auto wide = widen(utf8);
        DWORD written = 0;
        WriteConsoleW(h, wide.data(), static_cast<DWORD>(wide.size()), &written, nullptr);
        return;
    }
    auto* f = error_stream ? stderr : stdout;
    std::fwrite(utf8.data(), 1, utf8.size(), f);
    std::fflush(f);
}

} // namespace ode::ui
