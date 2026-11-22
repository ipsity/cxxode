#pragma once

#include <string>
#include <string_view>

namespace lay::platform {

std::string sha256(std::string_view bytes);
std::string random_suffix();

} // namespace lay::platform
