#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace ode {

// %USERPROFILE%; "." when it is unset so a broken environment still starts.
std::filesystem::path user_home();
// %USERPROFILE%\.ode, where all of ode's own state lives.
std::filesystem::path ode_home();

std::optional<std::string> read_file(const std::filesystem::path& path);

} // namespace ode
