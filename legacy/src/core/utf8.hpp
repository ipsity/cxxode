#pragma once

#include <cstdint>
#include <string_view>

namespace lay {

inline bool continuation(unsigned char byte) { return (byte & 0xc0) == 0x80; }

inline bool valid_utf8(std::string_view text) {
    for (std::size_t offset = 0; offset < text.size();) {
        const auto first = static_cast<unsigned char>(text[offset++]);
        if (first < 0x80) {
            continue;
        }
        const auto length = first < 0xe0 ? 2u : first < 0xf0 ? 3u : 4u;
        if (first < 0xc2 || first > 0xf4 || length - 1 > text.size() - offset) {
            return false;
        }
        std::uint32_t scalar = first & (0x7f >> length);
        for (unsigned int i = 1; i < length; ++i) {
            const auto byte = static_cast<unsigned char>(text[offset++]);
            if (!continuation(byte)) {
                return false;
            }
            scalar = (scalar << 6) | (byte & 0x3f);
        }
        if ((length == 3 && scalar < 0x800) || (length == 4 && scalar < 0x10000) ||
            (scalar >= 0xd800 && scalar <= 0xdfff) || scalar > 0x10ffff) {
            return false;
        }
    }
    return true;
}

inline std::string_view utf8_prefix(std::string_view value, std::size_t size) {
    if (size >= value.size()) {
        return value;
    }
    while (size > 0 && continuation(static_cast<unsigned char>(value[size]))) {
        --size;
    }
    return value.substr(0, size);
}

} // namespace lay
