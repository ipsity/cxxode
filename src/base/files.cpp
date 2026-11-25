#include "base/files.hpp"

#include <cstdlib>
#include <fstream>
#include <sstream>

namespace ode {

std::filesystem::path user_home() {
    const char* home = std::getenv("USERPROFILE");
    return std::filesystem::path(home && *home ? home : ".");
}

std::filesystem::path ode_home() { return user_home() / ".ode"; }

std::optional<std::string> read_file(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    std::ostringstream out;
    out << in.rdbuf();
    return std::move(out).str();
}

} // namespace ode
