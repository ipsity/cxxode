#include "base/text.hpp"

namespace ode {

std::string clip(std::string text, std::size_t limit) {
    if (text.size() <= limit) return text;
    const auto half = limit / 2;
    // A tiny limit has no room for both ends and a marker, so just cut the tail.
    if (half == 0) {
        auto end = limit;
        while (end > 0 && (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) --end;
        return text.substr(0, end) + "…";
    }
    auto head = half, tail = text.size() - half;
    // Never cut inside a UTF-8 sequence.
    while (head > 0 && (static_cast<unsigned char>(text[head]) & 0xC0) == 0x80) --head;
    while (tail < text.size() && (static_cast<unsigned char>(text[tail]) & 0xC0) == 0x80) ++tail;
    return text.substr(0, head) + "\n… [" + std::to_string(tail - head) + " bytes omitted] …\n" + text.substr(tail);
}

} // namespace ode
