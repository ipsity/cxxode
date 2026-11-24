#include "ui/render.hpp"

namespace ode::ui {

bool& color_enabled() {
    static bool enabled = true;
    return enabled;
}

std::string sgr(tone t) {
    if (!color_enabled()) return {};
    switch (t) {
        case tone::normal: return "\x1b[0m";
        case tone::bold: return "\x1b[0;1m";
        case tone::italic: return "\x1b[0;3m";
        case tone::code: return "\x1b[0;38;2;0;212;255m";
        case tone::dim: return "\x1b[0;90m";
        case tone::accent: return "\x1b[0;38;2;0;212;255m";
        case tone::accent_bold: return "\x1b[0;1;38;2;0;212;255m";
        case tone::error: return "\x1b[0;38;2;255;95;95m";
    }
    return {};
}

std::string paint(tone t, std::string_view s) {
    if (!color_enabled() || t == tone::normal) return std::string(s);
    return sgr(t) + std::string(s) + "\x1b[0m";
}

std::vector<std::string> layout(const std::vector<span>& spans, int columns, const prefix& first, const prefix& next) {
    std::vector<std::string> rows;
    std::string row = first.text;
    int used = first.width;
    int limit = columns;
    tone current = tone::normal;
    bool row_has_text = false;

    const auto finish_row = [&] {
        if (current != tone::normal && color_enabled()) row += "\x1b[0m";
        rows.push_back(std::move(row));
        row = next.text;
        used = next.width;
        row_has_text = false;
        if (current != tone::normal) row += sgr(current);
    };
    const auto set_tone = [&](tone t) {
        if (t == current) return;
        current = t;
        row += sgr(t);
    };

    for (const auto& s : spans) {
        std::size_t i = 0;
        const auto text = text::expand_tabs(s.text);
        while (i < text.size()) {
            if (text[i] == '\n') {
                finish_row();
                ++i;
                continue;
            }
            // A word is a run of non-space ASCII; every wide character is its own word.
            std::size_t j = i;
            if (text[i] == ' ') {
                while (j < text.size() && text[j] == ' ') ++j;
            } else if (static_cast<unsigned char>(text[i]) < 0x80) {
                while (j < text.size() && text[j] != ' ' && text[j] != '\n' && static_cast<unsigned char>(text[j]) < 0x80) ++j;
            } else {
                j = text::next_boundary(text, i);
                while (j < text.size() && (static_cast<unsigned char>(text[j]) & 0xC0) == 0x80) ++j;
            }
            const auto word = std::string_view(text).substr(i, j - i);
            const int w = text::width(word);
            const bool spaces = word.front() == ' ';
            if (used + w > limit && row_has_text) {
                finish_row();
                if (spaces) {
                    i = j;
                    continue;
                }
            }
            if (used + w > limit) {
                // Longer than a row: split by grapheme.
                for (const auto& c : text::cells(word)) {
                    const auto piece = word.substr(c.offset, text::next_boundary(word, c.offset) - c.offset);
                    if (used + c.width > limit && row_has_text) finish_row();
                    set_tone(s.style);
                    row += piece;
                    used += c.width;
                    row_has_text = true;
                }
            } else {
                set_tone(s.style);
                row += word;
                used += w;
                row_has_text = row_has_text || !spaces;
            }
            i = j;
        }
    }
    if (current != tone::normal && color_enabled()) row += "\x1b[0m";
    rows.push_back(std::move(row));
    return rows;
}

namespace {

std::vector<span> inline_spans(std::string_view s, tone base) {
    std::vector<span> out;
    std::string current;
    bool bold = false, italic = false, code = false;
    const auto flush = [&] {
        if (current.empty()) return;
        tone t = code ? tone::code : bold ? tone::bold : italic ? tone::italic : base;
        out.push_back({std::move(current), t});
        current.clear();
    };
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '`') {
            flush();
            code = !code;
        } else if (!code && s.substr(i).starts_with("**")) {
            flush();
            bold = !bold;
            ++i;
        } else if (!code && (s[i] == '*' || s[i] == '_')) {
            // Single * or _ toggles italic (but not inside code or bold)
            if (!bold) {
                flush();
                italic = !italic;
            } else {
                current += s[i];
            }
        } else if (!code && s[i] == '[') {
            // Simplified link: [text](url) → just show text
            std::size_t text_end = i + 1;
            while (text_end < s.size() && s[text_end] != ']') ++text_end;
            if (text_end < s.size() && text_end + 1 < s.size() && s[text_end + 1] == '(') {
                std::size_t url_end = text_end + 2;
                while (url_end < s.size() && s[url_end] != ')') ++url_end;
                if (url_end < s.size()) {
                    // Valid link: extract text only
                    flush();
                    current = std::string(s.substr(i + 1, text_end - i - 1));
                    flush();
                    i = url_end;
                    continue;
                }
            }
            current += s[i];
        } else {
            current += s[i];
        }
    }
    flush();
    return out;
}

} // namespace

std::vector<std::string> markdown_line(std::string_view line, markdown_state& state, int columns) {
    std::string_view trimmed = line;
    while (!trimmed.empty() && trimmed.front() == ' ') trimmed.remove_prefix(1);
    if (trimmed.starts_with("```")) {
        state.code = !state.code;
        return {};
    }
    const prefix indent{"  ", 2};
    if (state.code) {
        return layout({{std::string(line), tone::code}}, columns, {"  │ ", 4}, {"  │ ", 4});
    }
    if (trimmed.empty()) return {""};
    if (trimmed.starts_with("#")) {
        auto h = trimmed;
        while (!h.empty() && h.front() == '#') h.remove_prefix(1);
        while (!h.empty() && h.front() == ' ') h.remove_prefix(1);
        return layout({{std::string(h), tone::bold}}, columns, indent, indent);
    }
    const auto lead = line.size() - trimmed.size();
    if (trimmed.starts_with("- ") || trimmed.starts_with("* ")) {
        const std::string pad(lead, ' ');
        const prefix bullet{"  " + pad + "• ", 4 + static_cast<int>(lead)};
        const prefix hang{"  " + pad + "  ", 4 + static_cast<int>(lead)};
        return layout(inline_spans(trimmed.substr(2), tone::normal), columns, bullet, hang);
    }
    std::size_t digits = 0;
    while (digits < trimmed.size() && trimmed[digits] >= '0' && trimmed[digits] <= '9') ++digits;
    if (digits && digits + 1 < trimmed.size() && trimmed[digits] == '.' && trimmed[digits + 1] == ' ') {
        const auto number = std::string(trimmed.substr(0, digits + 2));
        const int w = 2 + static_cast<int>(lead + number.size());
        return layout(inline_spans(trimmed.substr(digits + 2), tone::normal), columns,
                      {"  " + std::string(lead, ' ') + number, w}, {std::string(static_cast<std::size_t>(w), ' '), w});
    }
    if (trimmed.starts_with("> ")) {
        return layout(inline_spans(trimmed.substr(2), tone::dim), columns, {"  " + paint(tone::dim, "│ "), 4},
                      {"  " + paint(tone::dim, "│ "), 4});
    }
    return layout(inline_spans(line, tone::normal), columns, indent, indent);
}

std::string_view verb(std::string_view tool) {
    if (tool == "Read") return "读";
    if (tool == "Write") return "写";
    if (tool == "Edit") return "改";
    if (tool == "Bash") return "跑";
    if (tool == "Grep") return "搜";
    if (tool == "Glob") return "找";
    return tool;
}

std::string one_line(std::string_view s) {
    std::string out;
    for (const char c : s) out += c == '\n' || c == '\r' || c == '\t' ? ' ' : c;
    return out;
}

} // namespace ode::ui
