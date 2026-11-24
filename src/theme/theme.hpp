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

    // Cached ANSI codes (for performance)
    std::string user_message_fg;
    std::string assistant_message_fg;
    std::string tool_read_fg;
    std::string tool_write_fg;
    std::string tool_edit_fg;
    std::string tool_bash_fg;
    std::string tool_search_fg;
    std::string prompt_fg;
    std::string border_fg;
    std::string accent_fg;
    std::string error_fg;
    std::string success_fg;

    // Update cached codes after color changes
    void update_cache() {
        user_message_fg = user_message.ansi_fg();
        assistant_message_fg = assistant_message.ansi_fg();
        tool_read_fg = tool_read.ansi_fg();
        tool_write_fg = tool_write.ansi_fg();
        tool_edit_fg = tool_edit.ansi_fg();
        tool_bash_fg = tool_bash.ansi_fg();
        tool_search_fg = tool_search.ansi_fg();
        prompt_fg = prompt.ansi_fg();
        border_fg = border.ansi_fg();
        accent_fg = accent.ansi_fg();
        error_fg = error.ansi_fg();
        success_fg = success.ansi_fg();
    }
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
