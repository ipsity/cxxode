#pragma once

#include "platform/crypto.hpp"
#include "platform/workspace.hpp"

namespace lay {

inline constexpr std::size_t editable_bytes = 2 * 1024 * 1024;

inline std::string file_version(const platform::Node& node, std::string_view bytes) {
    return platform::sha256(node.identity() + "\n" + std::string(bytes));
}

} // namespace lay
