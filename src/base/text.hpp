#pragma once

#include <string>
#include <string_view>

namespace ode {

// Truncates to a character boundary. Short text is returned unchanged; text that is only
// slightly over the limit loses its tail; long text keeps both ends and says how much was cut.
std::string clip(std::string text, std::size_t limit);

} // namespace ode
