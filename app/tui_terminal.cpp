#include "tui_terminal.hpp"

#include <algorithm>
#include <stdexcept>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <cerrno>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#endif

namespace lay::tui {
namespace {

constexpr auto needs_terminal = "TUI requires an interactive terminal on stdin and stdout. Run lay-code --help for line mode.";
constexpr auto enter_screen = "\x1b[?1049h\x1b[?25l\x1b[?2004h";
constexpr auto leave_screen = "\x1b[?2004l\x1b[0m\x1b[?25h\x1b[?1049l";

#ifdef _WIN32
std::string utf8(const wchar_t* data, int count) {
    const auto length = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, data, count, nullptr, 0, nullptr, nullptr);
    std::string result(static_cast<std::size_t>(length), '\0');
    if (length) {
        WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, data, count, result.data(), length, nullptr, nullptr);
    }
    return result;
}

std::string_view control(const KEY_EVENT_RECORD& key) {
    switch (key.wVirtualKeyCode) {
        case VK_F2: return "\x1bOQ";
        case VK_F3: return "\x1bOR";
        case VK_F4: return "\x1bOS";
        case VK_UP: return "\x1b[A";
        case VK_DOWN: return "\x1b[B";
        case VK_RIGHT: return "\x1b[C";
        case VK_LEFT: return "\x1b[D";
        case VK_HOME: return "\x1b[H";
        case VK_END: return "\x1b[F";
        case VK_PRIOR: return "\x1b[5~";
        case VK_NEXT: return "\x1b[6~";
        case VK_DELETE: return "\x1b[3~";
        case VK_ESCAPE: return "\x1b";
        case VK_TAB: return "\t";
        case VK_RETURN: return "\r";
        case VK_BACK: return "\b";
        default: return {};
    }
}
#endif

} // namespace

struct Terminal::Native {
    bool restore_modes = false;
#ifdef _WIN32
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    HANDLE out = GetStdHandle(STD_OUTPUT_HANDLE);
    DWORD original_input = 0;
    DWORD original_output = 0;
    wchar_t high_surrogate = 0;

    ~Native() {
        if (restore_modes) {
            SetConsoleMode(in, original_input);
            SetConsoleMode(out, original_output);
        }
    }
#else
    termios original{};

    ~Native() {
        if (restore_modes) {
            tcsetattr(STDIN_FILENO, TCSANOW, &original);
        }
    }
#endif
};

Terminal::Terminal() : native_(std::make_unique<Native>()) {
#ifdef _WIN32
    auto& native = *native_;
    if (!GetConsoleMode(native.in, &native.original_input) || !GetConsoleMode(native.out, &native.original_output)) {
        throw std::invalid_argument(needs_terminal);
    }
    native.restore_modes = true;
    const auto input = (native.original_input | ENABLE_EXTENDED_FLAGS | ENABLE_WINDOW_INPUT) &
        ~(ENABLE_LINE_INPUT | ENABLE_ECHO_INPUT | ENABLE_PROCESSED_INPUT | ENABLE_QUICK_EDIT_MODE |
          ENABLE_MOUSE_INPUT | ENABLE_VIRTUAL_TERMINAL_INPUT);
    const auto output = native.original_output | ENABLE_PROCESSED_OUTPUT | ENABLE_VIRTUAL_TERMINAL_PROCESSING;
    if (!SetConsoleMode(native.in, input) || !SetConsoleMode(native.out, output)) {
        throw std::runtime_error("Could not enable interactive Windows console modes.");
    }
#else
    if (!isatty(STDIN_FILENO) || !isatty(STDOUT_FILENO) || tcgetattr(STDIN_FILENO, &native_->original) != 0) {
        throw std::invalid_argument(needs_terminal);
    }
    auto raw = native_->original;
    cfmakeraw(&raw);
    raw.c_cc[VMIN] = 1;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0) {
        throw std::runtime_error("Could not enable interactive terminal modes.");
    }
    native_->restore_modes = true;
#endif
    try {
        write(enter_screen);
    } catch (...) {
        try { write(leave_screen); } catch (...) {}
        throw;
    }
}

Terminal::~Terminal() {
    try { write(leave_screen); } catch (...) {}
}

Size Terminal::size() const {
#ifdef _WIN32
    CONSOLE_SCREEN_BUFFER_INFO info{};
    if (GetConsoleScreenBufferInfo(native_->out, &info)) {
        const auto& window = info.srWindow;
        return {window.Right - window.Left + 1, window.Bottom - window.Top + 1};
    }
#else
    winsize info{};
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &info) == 0 && info.ws_col && info.ws_row) {
        return {info.ws_col, info.ws_row};
    }
#endif
    return {80, 24};
}

std::optional<std::string> Terminal::read() {
#ifdef _WIN32
    auto& native = *native_;
    const auto wait = WaitForSingleObject(native.in, 50);
    if (wait == WAIT_TIMEOUT) {
        return std::string{};
    }
    INPUT_RECORD batch[128];
    DWORD count = 0;
    if (wait != WAIT_OBJECT_0 || !ReadConsoleInputW(native.in, batch, 128, &count)) {
        return std::nullopt;
    }
    std::string bytes;
    for (DWORD i = 0; i < count; ++i) {
        if (batch[i].EventType != KEY_EVENT || !batch[i].Event.KeyEvent.bKeyDown) {
            continue;
        }
        const auto& key = batch[i].Event.KeyEvent;
        auto mapped = control(key);
        const auto modifiers = key.dwControlKeyState;
        if ((modifiers & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) &&
            !(modifiers & (LEFT_ALT_PRESSED | RIGHT_ALT_PRESSED))) {
            if (key.wVirtualKeyCode == 'C') mapped = "\x03";
            if (key.wVirtualKeyCode == 'O') mapped = "\x0f";
        }
        std::string encoded;
        const auto unit = key.uChar.UnicodeChar;
        if (mapped.empty() && unit) {
            if (unit >= 0xd800 && unit <= 0xdbff) {
                native.high_surrogate = unit;
                continue;
            }
            if (native.high_surrogate && unit >= 0xdc00 && unit <= 0xdfff) {
                const wchar_t pair[]{native.high_surrogate, unit};
                encoded = utf8(pair, 2);
            } else {
                encoded = utf8(&unit, 1);
            }
            mapped = encoded;
        }
        native.high_surrogate = 0;
        for (int repeat = 0; repeat < std::min<int>(key.wRepeatCount, 32); ++repeat) {
            bytes += mapped;
        }
    }
    return bytes;
#else
    pollfd input{STDIN_FILENO, POLLIN, 0};
    const auto ready = poll(&input, 1, 50);
    if (ready == 0 || (ready < 0 && errno == EINTR)) {
        return std::string{};
    }
    char bytes[4096];
    const auto count = ready > 0 ? ::read(STDIN_FILENO, bytes, sizeof(bytes)) : -1;
    if (count <= 0) {
        return std::nullopt;
    }
    return std::string(bytes, static_cast<std::size_t>(count));
#endif
}

void Terminal::write(std::string_view bytes) const {
    if (bytes.empty()) {
        return;
    }
#ifdef _WIN32
    const auto size = static_cast<int>(bytes.size());
    const auto length = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), size, nullptr, 0);
    if (!length) {
        throw std::runtime_error("Could not encode terminal frame.");
    }
    std::wstring text(static_cast<std::size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, bytes.data(), size, text.data(), length);
    DWORD written = 0;
    if (!WriteConsoleW(native_->out, text.data(), static_cast<DWORD>(text.size()), &written, nullptr) ||
        written != text.size()) {
        throw std::runtime_error("Could not write terminal frame.");
    }
#else
    while (!bytes.empty()) {
        const auto written = ::write(STDOUT_FILENO, bytes.data(), bytes.size());
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            throw std::runtime_error("Could not write terminal frame.");
        }
        bytes.remove_prefix(static_cast<std::size_t>(written));
    }
#endif
}

} // namespace lay::tui
