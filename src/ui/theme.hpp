#pragma once

#include <map>
#include <string>

namespace ode::ui {

// Theme color definitions
struct theme_colors {
    std::string accent = "#00d4ff";
    std::string accent_dim = "#0088aa";
    std::string error = "#ff5f5f";
    std::string warning = "#ffaa00";
    std::string success = "#00ff88";
    std::string dim = "#6c7086";
    std::string border = "#3c3c3c";
};

// Theme definition
struct theme {
    std::string name;
    bool dark_mode = true;
    theme_colors colors;
};

// Built-in themes
const theme& get_theme(const std::string& name);
const theme& auto_detect_theme();

// Available built-in themes
std::map<std::string, theme> builtin_themes();

} // namespace ode::ui
