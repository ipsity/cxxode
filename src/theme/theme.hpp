#pragma once

#include <cstdint>
#include <string>
#include <map>

namespace ode {

struct rgb_color {
    uint8_t r, g, b;

    std::string ansi_fg() const;
    std::string ansi_bg() const;
};

struct theme {
    std::string name;

    // Message colors
    rgb_color user_message;
    rgb_color assistant_message;

    // Tool colors
    rgb_color tool_read;
    rgb_color tool_write;
    rgb_color tool_edit;
    rgb_color tool_bash;
    rgb_color tool_search;

    // UI colors
    rgb_color prompt;
    rgb_color border;
    rgb_color dim;
    rgb_color accent;
    rgb_color error;
    rgb_color success;
    rgb_color warning;
    rgb_color info;

    // Background (for reference)
    rgb_color background;
};

// Predefined themes
theme dark_theme();
theme light_theme();
theme solarized_dark();
theme dracula();
theme nord();
theme monokai();

// Theme management
std::map<std::string, theme> available_themes();
theme load_theme(const std::string& name);
bool save_theme_selection(const std::string& theme_name);
std::string load_theme_selection();

} // namespace ode
