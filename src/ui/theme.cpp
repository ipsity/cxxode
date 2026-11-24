#include "ui/theme.hpp"

namespace ode::ui {

std::map<std::string, theme> builtin_themes() {
    std::map<std::string, theme> themes;

    // Default dark theme (current)
    themes["default-dark"] = {
        "default-dark",
        true,
        {
            "#00d4ff",  // accent (cyan)
            "#0088aa",  // accent_dim
            "#ff5f5f",  // error (red)
            "#ffaa00",  // warning (orange)
            "#00ff88",  // success (green)
            "#6c7086",  // dim (gray)
            "#3c3c3c"   // border
        }
    };

    // Light theme
    themes["default-light"] = {
        "default-light",
        false,
        {
            "#0066cc",  // accent (blue)
            "#004488",  // accent_dim
            "#cc0000",  // error (red)
            "#cc8800",  // warning (orange)
            "#00aa44",  // success (green)
            "#888888",  // dim (gray)
            "#cccccc"   // border
        }
    };

    // Minimal (no colors, only styles)
    themes["minimal"] = {
        "minimal",
        true,
        {
            "",  // accent (use default)
            "",  // accent_dim
            "",  // error
            "",  // warning
            "",  // success
            "",  // dim
            ""   // border
        }
    };

    // Nord theme
    themes["nord"] = {
        "nord",
        true,
        {
            "#88c0d0",  // accent (nord8 - frost)
            "#5e81ac",  // accent_dim (nord10)
            "#bf616a",  // error (nord11 - red)
            "#ebcb8b",  // warning (nord13 - yellow)
            "#a3be8c",  // success (nord14 - green)
            "#4c566a",  // dim (nord3)
            "#3b4252"   // border (nord1)
        }
    };

    // GitHub theme
    themes["github"] = {
        "github",
        false,
        {
            "#0969da",  // accent (blue)
            "#0550ae",  // accent_dim
            "#cf222e",  // error (red)
            "#9a6700",  // warning (orange)
            "#1a7f37",  // success (green)
            "#656d76",  // dim (gray)
            "#d0d7de"   // border
        }
    };

    return themes;
}

const theme& get_theme(const std::string& name) {
    static const auto themes = builtin_themes();
    auto it = themes.find(name);
    if (it != themes.end()) {
        return it->second;
    }
    // Default to default-dark
    return themes.at("default-dark");
}

const theme& auto_detect_theme() {
    // Try to detect terminal background
    // For now, default to dark theme
    // TODO: Implement OSC 11 query or COLORFGBG parsing
    return get_theme("default-dark");
}

} // namespace ode::ui
