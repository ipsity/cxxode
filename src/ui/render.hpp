#pragma once

#include "ui/text.hpp"

#include <string>
#include <string_view>
#include <vector>

namespace ode::ui {

enum class tone { normal, bold, italic, code, dim, accent, accent_bold, error, diff_added, diff_removed };

bool& color_enabled();
std::string sgr(tone t);
std::string paint(tone t, std::string_view s);

struct span {
    std::string text;
    tone style = tone::normal;
};

struct prefix {
    std::string text;  // may contain escape sequences
    int width = 0;
};

// Lays out styled text into rows no wider than `columns`, breaking between words
// and between wide characters. Rows after the first start with `next`.
std::vector<std::string> layout(const std::vector<span>& spans, int columns, const prefix& first, const prefix& next);

struct markdown_state {
    bool code = false;
};

// Renders one logical Markdown line. Returns no rows for code fences.
std::vector<std::string> markdown_line(std::string_view line, markdown_state& state, int columns);

// Chinese verb for a tool name: Read → 读.
std::string_view verb(std::string_view tool);
// Single-line form of a label: newlines become spaces.
std::string one_line(std::string_view s);
std::string clip_styled_line(std::string_view line, int columns);

} // namespace ode::ui
