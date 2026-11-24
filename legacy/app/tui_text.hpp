#pragma once

#include "core/utf8.hpp"

#include <algorithm>
#include <string>
#include <utility>
#include <vector>

namespace lay::tui {

inline std::size_t scalar_bytes(unsigned char first) {
    return first < 0x80 ? 1 : first < 0xe0 ? 2 : first < 0xf0 ? 3 : 4;
}

inline std::uint32_t scalar(std::string_view bytes) {
    auto value = static_cast<std::uint32_t>(static_cast<unsigned char>(bytes.front()));
    if (bytes.size() > 1) {
        value &= 0x7f >> bytes.size();
        for (const auto byte : bytes.substr(1)) {
            value = (value << 6) | (static_cast<unsigned char>(byte) & 0x3f);
        }
    }
    return value;
}

struct Cell {
    std::size_t offset;
    int width;
};

inline std::vector<Cell> cells(std::string_view text) {
    std::vector<Cell> result;
    bool joined = false;
    bool regional = false;
    for (std::size_t offset = 0; offset < text.size();) {
        const auto length = scalar_bytes(static_cast<unsigned char>(text[offset]));
        const auto code = scalar(text.substr(offset, length));
        const bool emoji = code >= 0x1f000 && code <= 0x1faff;
        const bool flag = code >= 0x1f1e6 && code <= 0x1f1ff;
        // Common combining marks, variation selectors, emoji modifiers and ZWJ.
        const bool extend = (code >= 0x0300 && code <= 0x036f) || (code >= 0x1ab0 && code <= 0x1aff) ||
            (code >= 0x1dc0 && code <= 0x1dff) || (code >= 0x20d0 && code <= 0x20ff) ||
            (code >= 0xfe00 && code <= 0xfe0f) || (code >= 0xfe20 && code <= 0xfe2f) ||
            (code >= 0x1f3fb && code <= 0x1f3ff) || (code >= 0xe0100 && code <= 0xe01ef) || code == 0x200d;
        const bool wide = emoji || (code >= 0x1100 && code <= 0x115f) ||
            (code >= 0x2e80 && code <= 0xa4cf) || (code >= 0xac00 && code <= 0xd7a3) ||
            (code >= 0xf900 && code <= 0xfaff) || (code >= 0xff01 && code <= 0xff60) ||
            (code >= 0x20000 && code <= 0x3fffd);
        const bool attach = !result.empty() && (extend || joined || (flag && regional));
        if (attach) {
            if ((!extend && wide) || code == 0xfe0f || code == 0x20e3) {
                result.back().width = 2;
            }
        } else {
            result.push_back({offset, wide ? 2 : 1});
        }
        joined = code == 0x200d;
        regional = flag && !regional;
        offset += length;
    }
    result.push_back({text.size(), 0});
    return result;
}

inline std::vector<std::string> wrap(std::string_view text, int width) {
    const auto boundaries = cells(text);
    std::vector<std::string> rows;
    std::size_t start = 0;
    int columns = 0;
    for (const auto cell : boundaries) {
        if (columns + cell.width > width && cell.offset > start) {
            rows.emplace_back(text.substr(start, cell.offset - start));
            start = cell.offset;
            columns = 0;
        }
        columns += cell.width;
    }
    rows.emplace_back(text.substr(start));
    return rows;
}

class Draft {
public:
    static constexpr std::size_t max_bytes = 4096;
    std::string text;
    std::size_t cursor = 0;

    bool insert(std::string_view value) {
        if (text.size() + value.size() > max_bytes) return false;
        text.insert(cursor, value);
        cursor += value.size();
        if (cursor != text.size()) {
            const auto boundaries = cells(text);
            cursor = std::ranges::lower_bound(boundaries, cursor, {}, &Cell::offset)->offset;
        }
        return true;
    }

    void move(bool forward, bool erase) {
        const auto boundaries = cells(text);
        const auto current = std::ranges::lower_bound(boundaries, cursor, {}, &Cell::offset);
        const auto next = forward ? (current + 1 == boundaries.end() ? current : current + 1) :
                                    (current == boundaries.begin() ? current : current - 1);
        if (!erase) {
            cursor = next->offset;
            return;
        }
        const auto first = std::min(cursor, next->offset);
        text.erase(first, std::max(cursor, next->offset) - first);
        const auto remaining = cells(text);
        cursor = std::ranges::lower_bound(remaining, first, {}, &Cell::offset)->offset;
    }

    std::pair<std::string, int> view(int width, bool ascii) const {
        const auto boundaries = cells(text);
        const auto current = std::ranges::lower_bound(boundaries, cursor, {}, &Cell::offset);
        auto start = current;
        int column = 0;
        while (start != boundaries.begin() && column + (start - 1)->width < width) {
            column += (--start)->width;
        }
        std::string output;
        int rendered = 0;
        for (auto item = start; item + 1 != boundaries.end() && rendered + item->width <= width; ++item) {
            const auto cluster = std::string_view(text).substr(item->offset, (item + 1)->offset - item->offset);
            if (ascii && !std::ranges::all_of(cluster, [](unsigned char value) { return value < 0x80; })) {
                output.append(static_cast<std::size_t>(item->width), '?');
            } else {
                output += cluster;
            }
            rendered += item->width;
        }
        return {std::move(output), column};
    }
};

} // namespace lay::tui
