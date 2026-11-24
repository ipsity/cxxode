#pragma once

#include "ui/unicode.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ode::text {

inline std::size_t scalar_bytes(unsigned char first) {
    return first < 0x80 ? 1 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
}

inline std::uint32_t scalar(std::string_view bytes) {
    auto value = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes.front()));
    if (bytes.size() > 1) {
        value &= 0x7fu >> bytes.size();
        for (const auto byte : bytes.substr(1)) value = (value << 6) | (static_cast<unsigned char>(byte) & 0x3f);
    }
    return value;
}

// One grapheme cluster: where it starts and how many terminal columns it takes.
struct cell {
    std::size_t offset;
    int width;
};

inline std::vector<cell> cells(std::string_view s) {
    std::vector<cell> out;
    unicode::Segmenter segmenter;
    const bool wide = unicode::cjk_width();
    std::optional<cell> pending;
    for (std::size_t i = 0; i < s.size();) {
        const auto n = std::min(scalar_bytes(static_cast<unsigned char>(s[i])), s.size() - i);
        const auto code = scalar(s.substr(i, n));
        const auto props = unicode::properties(code);
        const auto kind = props.grapheme();
        const bool visible = kind != unicode::Break::extend && kind != unicode::Break::zwj && kind != unicode::Break::prepend;
        if (segmenter.boundary(props) || !pending) {
            if (pending) out.push_back(*pending);
            int w = visible && props.double_width(wide) ? 2 : 1;
            if (code == '\t') w = 4;
            else if (code < 0x20 || code == 0x7f) w = 0;
            pending = cell{i, w};
        } else if ((visible && props.double_width(wide)) || code == 0xfe0f || code == 0x20e3) {
            pending->width = 2;
        }
        i += n;
    }
    if (pending) out.push_back(*pending);
    return out;
}

// Fast ASCII check for optimization
inline bool is_ascii(std::string_view s) {
    return std::all_of(s.begin(), s.end(),
        [](char c) { return static_cast<unsigned char>(c) < 128; });
}

// String width with caching and ASCII fast path
inline int width(std::string_view s) {
    // Fast path for ASCII strings
    if (is_ascii(s)) {
        return static_cast<int>(s.length());
    }

    // For non-ASCII, use full Unicode width calculation
    int total = 0;
    for (const auto& c : cells(s)) total += c.width;
    return total;
}

// Legacy version without caching (kept for compatibility)
inline int width_full(std::string_view s) {
    int w = 0;
    for (const auto& c : cells(s)) w += c.width;
    return w;
}

inline std::size_t prev_boundary(std::string_view s, std::size_t at) {
    std::size_t best = 0;
    for (const auto& c : cells(s)) {
        if (c.offset >= at) break;
        best = c.offset;
    }
    return best;
}

inline std::size_t next_boundary(std::string_view s, std::size_t at) {
    for (const auto& c : cells(s)) {
        if (c.offset > at) return c.offset;
    }
    return s.size();
}

inline std::string expand_tabs(std::string_view s) {
    std::string out;
    for (const char c : s) {
        if (c == '\t') out += "    ";
        else if (c != '\r') out += c;
    }
    return out;
}

// Wraps one logical line to rows of at most `columns`, preferring breaks after spaces.
inline std::vector<std::string> wrap_line(std::string_view raw, int columns) {
    const auto line = expand_tabs(raw);
    std::vector<std::string> rows;
    if (columns < 2) columns = 2;
    const auto cs = cells(line);
    std::size_t start = 0, last_space = std::string::npos;
    int used = 0, used_at_space = 0;
    for (std::size_t i = 0; i < cs.size(); ++i) {
        const auto& c = cs[i];
        if (used + c.width > columns && c.offset > start) {
            std::size_t cut = c.offset;
            int carried = 0;
            if (last_space != std::string::npos && last_space > start && used_at_space * 2 > columns) {
                cut = last_space;
                carried = used - used_at_space;
            }
            rows.emplace_back(line.substr(start, cut - start));
            start = cut;
            used = carried;
            last_space = std::string::npos;
        }
        used += c.width;
        if (line[c.offset] == ' ') {
            last_space = c.offset + 1;
            used_at_space = used;
        }
    }
    rows.emplace_back(line.substr(start));
    return rows;
}

inline std::vector<std::string> wrap(std::string_view s, int columns) {
    std::vector<std::string> rows;
    std::size_t start = 0;
    while (true) {
        const auto end = s.find('\n', start);
        for (auto& r : wrap_line(s.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start), columns))
            rows.push_back(std::move(r));
        if (end == std::string_view::npos) break;
        start = end + 1;
    }
    return rows;
}

// Keeps the end of s within `columns`, replacing the head with an ellipsis.
inline std::string keep_tail(std::string_view s, int columns) {
    if (width(s) <= columns) return std::string(s);
    const auto cs = cells(s);
    int used = 1;
    std::size_t i = cs.size();
    while (i > 0 && used + cs[i - 1].width <= columns) used += cs[--i].width;
    return "…" + std::string(s.substr(i < cs.size() ? cs[i].offset : s.size()));
}

// Keeps the start of s within `columns`, ending with an ellipsis.
inline std::string keep_head(std::string_view s, int columns) {
    if (width(s) <= columns) return std::string(s);
    int used = 1;
    std::size_t end = 0;
    const auto cs = cells(s);
    for (std::size_t i = 0; i < cs.size(); ++i) {
        if (used + cs[i].width > columns) break;
        used += cs[i].width;
        end = i + 1 < cs.size() ? cs[i + 1].offset : s.size();
    }
    return std::string(s.substr(0, end)) + "…";
}

} // namespace ode::text
