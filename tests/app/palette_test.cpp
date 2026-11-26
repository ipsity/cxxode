#include "app/palette.hpp"

#include <doctest.h>

using ode::detail::first_visible;

TEST_CASE("five-row command palette keeps selection centered and pins at both ends") {
    constexpr std::size_t visible = 5;
    constexpr std::size_t count = 10;

    CHECK(first_visible(0, visible, count) == 0);
    CHECK(first_visible(1, visible, count) == 0);
    CHECK(first_visible(2, visible, count) == 0);
    CHECK(first_visible(3, visible, count) == 1);
    for (std::size_t item = 3; item <= 8; ++item) {
        const auto first = first_visible(item - 1, visible, count);
        CHECK(item - 1 - first == visible / 2);
    }
    CHECK(first_visible(8, visible, count) == 5);
}

TEST_CASE("command palette window handles lists shorter than five entries") {
    CHECK(first_visible(0, 5, 3) == 0);
    CHECK(first_visible(2, 5, 3) == 0);
    CHECK(first_visible(0, 0, 3) == 0);
}
