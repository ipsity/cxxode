#pragma once

// Helpers shared by the process implementations in src/platform. Not part of the layer's interface.

#include "base/utf.hpp"

#include <string>
#include <string_view>

namespace ode::win {

class handle {
public:
    explicit handle(HANDLE h = nullptr) : h_(h) {}
    ~handle() { reset(); }
    handle(const handle&) = delete;
    handle& operator=(const handle&) = delete;
    HANDLE get() const { return h_; }
    explicit operator bool() const { return h_ && h_ != INVALID_HANDLE_VALUE; }
    void reset(HANDLE h = nullptr) {
        if (*this) CloseHandle(h_);
        h_ = h;
    }
    // Hands ownership to the caller without closing.
    HANDLE release() {
        HANDLE h = h_;
        h_ = nullptr;
        return h;
    }
private:
    HANDLE h_;
};

// Quotes one argument the way CommandLineToArgvW parses it. `force` quotes even plain
// arguments, which keeps cmd.exe metacharacters such as & and | literal.
inline std::wstring quote(std::wstring_view argument, bool force = false) {
    if (!force && !argument.empty() && argument.find_first_of(L" \t\"") == std::wstring_view::npos)
        return std::wstring(argument);
    std::wstring result(1, L'"');
    std::size_t slashes = 0;
    for (const auto c : argument) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        result.append(c == L'"' ? slashes * 2 + 1 : slashes, L'\\');
        result += c;
        slashes = 0;
    }
    result.append(slashes * 2, L'\\');
    result += L'"';
    return result;
}

} // namespace ode::win
