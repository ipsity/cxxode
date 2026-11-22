#pragma once

#include "core/session.hpp"

namespace lay {

enum class Script { normal, truncated };

inline constexpr std::string_view fixture_path = "config/defaults.txt";

Generate scripted_model(Script script);

} // namespace lay
