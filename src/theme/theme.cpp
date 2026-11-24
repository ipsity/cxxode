#include "theme/theme.hpp"

#include <sstream>
#include <fstream>
#include <filesystem>
#include <cstdlib>

namespace ode {

std::string rgb_color::ansi_fg() const {
    std::ostringstream oss;
    oss << "\033[38;2;" << static_cast<int>(r) << ";"
        << static_cast<int>(g) << ";" << static_cast<int>(b) << "m";
    return oss.str();
}

std::string rgb_color::ansi_bg() const {
    std::ostringstream oss;
    oss << "\033[48;2;" << static_cast<int>(r) << ";"
        << static_cast<int>(g) << ";" << static_cast<int>(b) << "m";
    return oss.str();
}

theme dark_theme() {
    theme t;
    t.name = "dark";
    t.user_message = {0, 212, 255};
    t.assistant_message = {212, 212, 212};
    t.tool_read = {79, 193, 255};
    t.tool_write = {206, 145, 120};
    t.tool_edit = {220, 220, 170};
    t.tool_bash = {78, 201, 176};
    t.tool_search = {78, 201, 176};
    t.prompt = {0, 212, 255};
    t.border = {106, 106, 106};
    t.dim = {106, 106, 106};
    t.accent = {0, 212, 255};
    t.error = {244, 135, 113};
    t.success = {78, 201, 176};
    t.warning = {206, 145, 120};
    t.info = {79, 193, 255};
    t.background = {30, 30, 30};
    return t;
}

theme dracula() {
    theme t;
    t.name = "dracula";
    t.user_message = {139, 233, 253};
    t.assistant_message = {248, 248, 242};
    t.tool_read = {139, 233, 253};
    t.tool_write = {255, 184, 108};
    t.tool_edit = {241, 250, 140};
    t.tool_bash = {80, 250, 123};
    t.tool_search = {139, 233, 253};
    t.prompt = {189, 147, 249};
    t.border = {68, 71, 90};
    t.dim = {98, 114, 164};
    t.accent = {189, 147, 249};
    t.error = {255, 85, 85};
    t.success = {80, 250, 123};
    t.warning = {255, 184, 108};
    t.info = {139, 233, 253};
    t.background = {40, 42, 54};
    return t;
}

theme nord() {
    theme t;
    t.name = "nord";
    t.user_message = {136, 192, 208};
    t.assistant_message = {236, 239, 244};
    t.tool_read = {136, 192, 208};
    t.tool_write = {208, 135, 112};
    t.tool_edit = {235, 203, 139};
    t.tool_bash = {163, 190, 140};
    t.tool_search = {143, 188, 187};
    t.prompt = {136, 192, 208};
    t.border = {76, 86, 106};
    t.dim = {76, 86, 106};
    t.accent = {136, 192, 208};
    t.error = {191, 97, 106};
    t.success = {163, 190, 140};
    t.warning = {208, 135, 112};
    t.info = {136, 192, 208};
    t.background = {46, 52, 64};
    return t;
}

theme monokai() {
    theme t;
    t.name = "monokai";
    t.user_message = {102, 217, 239};
    t.assistant_message = {248, 248, 240};
    t.tool_read = {102, 217, 239};
    t.tool_write = {253, 151, 31};
    t.tool_edit = {230, 219, 116};
    t.tool_bash = {166, 226, 46};
    t.tool_search = {102, 217, 239};
    t.prompt = {249, 38, 114};
    t.border = {73, 72, 62};
    t.dim = {117, 113, 94};
    t.accent = {249, 38, 114};
    t.error = {249, 38, 114};
    t.success = {166, 226, 46};
    t.warning = {253, 151, 31};
    t.info = {102, 217, 239};
    t.background = {39, 40, 34};
    return t;
}

theme light_theme() {
    theme t;
    t.name = "light";
    t.user_message = {0, 102, 204};
    t.assistant_message = {51, 51, 51};
    t.tool_read = {0, 102, 204};
    t.tool_write = {204, 102, 0};
    t.tool_edit = {153, 153, 0};
    t.tool_bash = {0, 153, 102};
    t.tool_search = {0, 153, 153};
    t.prompt = {0, 102, 204};
    t.border = {170, 170, 170};
    t.dim = {136, 136, 136};
    t.accent = {0, 102, 204};
    t.error = {204, 51, 51};
    t.success = {0, 153, 102};
    t.warning = {204, 102, 0};
    t.info = {0, 102, 204};
    t.background = {255, 255, 255};
    return t;
}

theme solarized_dark() {
    theme t;
    t.name = "solarized-dark";
    t.user_message = {38, 139, 210};
    t.assistant_message = {147, 161, 161};
    t.tool_read = {38, 139, 210};
    t.tool_write = {203, 75, 22};
    t.tool_edit = {181, 137, 0};
    t.tool_bash = {133, 153, 0};
    t.tool_search = {42, 161, 152};
    t.prompt = {38, 139, 210};
    t.border = {88, 110, 117};
    t.dim = {88, 110, 117};
    t.accent = {38, 139, 210};
    t.error = {220, 50, 47};
    t.success = {133, 153, 0};
    t.warning = {203, 75, 22};
    t.info = {38, 139, 210};
    t.background = {0, 43, 54};
    return t;
}

std::map<std::string, theme> available_themes() {
    return {
        {"dark", dark_theme()},
        {"dracula", dracula()},
        {"nord", nord()},
        {"monokai", monokai()},
        {"light", light_theme()},
        {"solarized-dark", solarized_dark()},
    };
}

theme load_theme(const std::string& name) {
    auto themes = available_themes();
    auto it = themes.find(name);
    if (it != themes.end()) return it->second;
    return dark_theme();
}

bool save_theme_selection(const std::string& theme_name) {
    // On Windows, use USERPROFILE instead of HOME
    const char* home = std::getenv("USERPROFILE");
    if (!home) {
        home = std::getenv("HOME");
        if (!home) return false;
    }

    const std::filesystem::path ode_dir = std::filesystem::path(home) / ".ode";
    std::filesystem::create_directories(ode_dir);

    const auto theme_file = ode_dir / "theme";
    std::ofstream out(theme_file);
    if (!out) return false;

    out << theme_name << "\n";
    return true;
}

std::string load_theme_selection() {
    // On Windows, use USERPROFILE instead of HOME
    const char* home = std::getenv("USERPROFILE");
    if (!home) {
        home = std::getenv("HOME");
        if (!home) return "dark";
    }

    const std::filesystem::path theme_file = std::filesystem::path(home) / ".ode" / "theme";
    std::ifstream in(theme_file);
    if (!in) return "dark";

    std::string theme_name;
    std::getline(in, theme_name);

    auto themes = available_themes();
    if (themes.find(theme_name) != themes.end()) {
        return theme_name;
    }

    return "dark";
}

} // namespace ode
