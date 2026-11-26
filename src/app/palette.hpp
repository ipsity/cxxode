#pragma once

#include <algorithm>
#include <cstddef>

namespace ode::detail {

inline std::size_t first_visible(std::size_t selected, std::size_t visible, std::size_t count) {
    if (visible == 0 || count <= visible) return 0;
    const auto center = visible / 2;
    return std::min(selected > center ? selected - center : 0, count - visible);
}

} // namespace ode::detail
