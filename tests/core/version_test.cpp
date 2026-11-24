#include "core/version.hpp"

#include <doctest.h>

TEST_CASE("version is semantic") {
    const auto v = lay::version();
    CHECK(v == CXXODE_VERSION);
    CHECK(v.find('.') != std::string_view::npos);
}
