#include "app/interactive.hpp"
#include "ext/lua/host.hpp"

#include <cctype>

namespace ode {

using ui::paint;
using ui::tone;

namespace {

constexpr std::size_t menu_rows = 6;

std::size_t first_visible(std::size_t selected, std::size_t visible) {
    return selected < visible ? 0 : selected + 1 - visible;
}

// Aliases (/b, /c, ...) still work when typed but are left out so each command appears once.
const std::pair<const char*, const char*> builtin_commands[] = {
    {"/help", "快捷键与命令"},
    {"/ref", "快速查看编辑 (Read→Edit→Write)"},
    {"/search", "搜索并查看 (Grep→Read)"},
    {"/fix", "智能检查修复"},
    {"/test", "运行测试"},
    {"/build", "构建项目"},
    {"/run", "运行程序"},
    {"/commit", "智能提交"},
    {"/lint", "代码检查"},
    {"/login", "登录或更新 API 密钥"},
    {"/logout", "清除保存的 API 密钥"},
    {"/format", "代码格式化"},
    {"/doc", "生成文档"},
    {"/goal", "设置工作目标"},
    {"/compact", "压缩上下文"},
    {"/reload", "重新加载 Lua 扩展"},
    {"/model", "查看或切换模型"},
    {"/theme", "切换主题配色"},
    {"/mode", "切换权限档"},
    {"/resume", "恢复会话"},
    {"/undo", "撤销改动"},
    {"/clear", "开始新会话"},
    {"/cost", "查看用量"},
    {"/exit", "退出"},
};

// 命令快捷键映射（用于在面板中显示）
const std::pair<const char*, const char*> command_shortcuts[] = {
    {"/search", "Alt+S"},
    {"/test", "Alt+T"},
    {"/build", "Alt+B"},
    {"/run", "Alt+R"},
    {"/commit", "Alt+C"},
    {"/help", "F1"},
    {"/clear", "Ctrl+N"},
};

std::string spinner(steady::time_point since) {
    // Unified dot rotation for all tools (Braille pattern)
    static constexpr const char* frames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧"};
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(steady::now() - since).count();
    return frames[(ms / 100) % 8];
}

std::string spinner_with_color(steady::time_point since, std::string_view tool_name, const theme& t) {
    // Use cached color codes for performance
    const std::string* color_code;

    if (tool_name == "Read") {
        color_code = &t.tool_read_fg;
    } else if (tool_name == "Write") {
        color_code = &t.tool_write_fg;
    } else if (tool_name == "Edit") {
        color_code = &t.tool_edit_fg;
    } else if (tool_name == "Bash") {
        color_code = &t.tool_bash_fg;
    } else if (tool_name == "Grep" || tool_name == "Glob") {
        color_code = &t.tool_search_fg;
    } else {
        static const std::string default_color = "\033[38;2;200;200;200m";
        color_code = &default_color;
    }

    return *color_code + spinner(since) + "\033[0m";
}

std::string seconds(steady::time_point since) {
    const auto s = std::chrono::duration_cast<std::chrono::seconds>(steady::now() - since).count();
    return s < 60 ? std::to_string(s) + "s" : std::to_string(s / 60) + "m" + std::to_string(s % 60) + "s";
}

} // namespace

std::vector<interactive_ui::palette_item> interactive_ui::palette_items() const {
    const auto& draft = editor_.text();
    if (busy_ || selection_ || login_ || !draft.starts_with('/') || draft.find_first_of(" \t\r\n") != std::string::npos) return {};
    const auto query = std::string_view(draft).substr(1);
    const auto score = [query](std::string_view name) -> int {
        if (query.empty()) return 100;
        name.remove_prefix(1);
        if (name == query) return 1100;
        if (name.starts_with(query)) return 1000;
        std::size_t matched = 0;
        int rank = 0;
        for (std::size_t i = 0; i < name.size() && matched < query.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(name[i])) == std::tolower(static_cast<unsigned char>(query[matched]))) {
                rank += i == 0 || name[i - 1] == ' ' ? 15 : 10;
                ++matched;
            }
        }
        return matched == query.size() ? rank : -1;
    };

    std::vector<palette_item> items;
    const auto add = [&](std::string name, std::string description, command_kind kind) {
        const auto rank = score(name);
        if (rank >= 0) items.push_back({std::move(name), std::move(description), kind, rank});
    };
    for (const auto& [name, about] : builtin_commands) {
        std::string description = about;
        for (const auto& [command, key] : command_shortcuts) {
            if (std::string_view(name) == command) description += " [" + std::string(key) + "]";
        }
        add(name, std::move(description), command_kind::builtin);
    }
    for (const auto& skill : app_.skills) {
        add("/" + skill.name, skill.description + " (skill)", command_kind::skill);
    }
    for (const auto& cmd : app_.commands) {
        auto about = cmd.argument_hint;
        if (!about.empty() && !cmd.description.empty()) about += "  ";
        about += cmd.description;
        add("/" + cmd.name, std::move(about), command_kind::user_command);
    }
    if (app_.lua) {
        for (const auto& name : app_.lua->commands()) {
            const auto slash = "/" + name;
            if (std::ranges::find(items, slash, &palette_item::name) == items.end()) {
                add(slash, "Lua", command_kind::user_command);
            }
        }
    }

    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.kind != b.kind) return a.kind < b.kind;
        return a.name < b.name;
    });
    return items;
}

std::vector<std::string> interactive_ui::suggestion_items() {
    const auto& input = editor_.text();

    // Don't suggest for empty input or commands
    if (input.empty() || input.front() == '/') return {};

    // Don't suggest when busy or asking
    if (busy_ || ask_) return {};

    // Get suggestions from engine
    auto suggestions = app_.suggestion_engine.analyze(input);

    // Format as strings: "tool_name - reason"
    std::vector<std::string> result;
    for (const auto& sugg : suggestions) {
        result.push_back("💡 " + sugg.tool_name + " - " + sugg.reason);
    }

    return result;
}

std::vector<std::string> interactive_ui::palette_rows(int columns) {
    std::vector<std::string> rows;
    const auto items = palette_items();
    const auto capacity = static_cast<std::size_t>(std::max(0, size_.rows - 5));
    const auto visible = std::min({menu_rows, items.size(), capacity});
    if (visible == 0) return rows;
    if (palette_index_ >= items.size()) palette_index_ = 0;

    const auto first = first_visible(palette_index_, visible);
    constexpr std::string_view marker = "▶ ";
    const int prefix_width = text::width(marker);
    for (auto i = first; i < first + visible; ++i) {
        const bool selected = i == palette_index_;
        const auto name = text::keep_head(ui::one_line(items[i].name), columns - prefix_width);
        const int name_width = text::width(name);
        const int gap = std::max(2, 12 - name_width);
        const auto description = text::keep_head(ui::one_line(items[i].description),
                                                 columns - prefix_width - name_width - gap);
        auto row = (selected ? std::string(marker) : std::string(prefix_width, ' ')) +
                   paint(selected ? tone::accent_bold : tone::normal, name);
        if (!description.empty()) row += std::string(gap, ' ') + paint(tone::dim, description);
        rows.push_back(std::move(row));
    }
    if (items.size() > visible) {
        rows.push_back("  " + paint(tone::dim,
            "[" + std::to_string(palette_index_ + 1) + "/" + std::to_string(items.size()) + "]"));
    }
    return rows;
}

std::string interactive_ui::status_line(int columns) const {
    const auto mode = app_.core ? app_.permissions.mode() : permission_mode::normal;
    std::string right = std::string(mode_name(mode)) + " · " + app_.provider_cfg.name + " / " + app_.core->model;
    if (usage_.context) {
        right += " · " + short_count(usage_.context) + "/" + short_count(app_.core->context_window);
        // Add cost display
        const double cost = app_.core->total.cost_usd(app_.core->model);
        if (cost >= 0.01) {
            right += " · $" + (cost < 1.0 ?
                std::to_string(static_cast<int>(cost * 100 + 0.5)) + "¢" :
                std::to_string(cost).substr(0, std::to_string(cost).find('.') + 3));
        }
    }
    const auto mode_tone = mode == permission_mode::yolo ? tone::error : mode == permission_mode::plan ? tone::accent : tone::dim;
    std::string left = "ode  " + home_relative(app_.root);
    if (!branch_.empty()) left += "  " + branch_;
    const int room = columns - 2 - text::width(right) - 2;
    if (room > 8 && app_.lua) {
        if (const auto status = app_.lua->status()) {
            const std::string separator = " · ";
            const int status_room = room - text::width(left) - text::width(separator);
            if (status_room > 0) {
                left += separator + text::keep_head(ui::one_line(*status), status_room);
            }
        }
    }
    if (room > 8 && text::width(left) > room) left = text::keep_tail(left, room);
    else if (room <= 8) left.clear();
    const int gap = std::max(1, columns - 2 - text::width(left) - text::width(right));
    const auto mode_text = std::string(mode_name(mode));
    return "  " + paint(tone::dim, left) + std::string(static_cast<std::size_t>(gap), ' ') + paint(mode_tone, mode_text) +
           paint(tone::dim, right.substr(mode_text.size()));
}

std::vector<std::string> interactive_ui::activity_rows() const {
    std::vector<std::string> rows;
    const int columns = std::max(1, size_.columns - 1);

    if (!think_stream_.empty()) {
        for (auto& r : thinking_rows(think_stream_, columns)) rows.push_back(std::move(r));
    }
    // Unfinished streamed line.
    if (!stream_.empty()) {
        auto state = md_;
        for (auto& r : ui::markdown_line(stream_, state, columns)) rows.push_back(std::move(r));
    }
    for (const auto& t : running_) {
        const auto head = spinner_with_color(t.started, t.name, app_.current_theme) + " " + std::string(ui::verb(t.name)) + " ";
        const auto tail = "  " + seconds(t.started);
        const auto label = text::keep_head(t.label, std::max(8, columns - 4 - text::width(head) - text::width(tail)));
        rows.push_back(" " + paint(tone::accent, head) + label + paint(tone::dim, tail));
    }
    if (!progress_tail_.empty()) {
        auto lines = text::wrap(progress_tail_, columns - 6);
        while (!lines.empty() && lines.back().empty()) lines.pop_back();
        const std::size_t keep = 4;
        for (std::size_t i = lines.size() > keep ? lines.size() - keep : 0; i < lines.size(); ++i)
            rows.push_back("   " + paint(tone::dim, "│ " + lines[i]));
    }
    if (!ask_ && busy_ && running_.empty() && (thinking_ || stream_.empty())) {
        rows.push_back(" " + paint(tone::accent, spinner_with_color(busy_since_, "Thinking", app_.current_theme)) + paint(tone::dim, " 思考中 " + seconds(busy_since_) + "  esc 中断"));
    }
    return rows;
}

std::vector<std::string> interactive_ui::footer_rows(int& cursor_row, int& cursor_col) {
    const int columns = std::max(1, size_.columns - 1);
    std::vector<std::string> rows;
    if (login_) {
        cursor_row = -1;
        cursor_col = 0;
        const auto visible = std::min(login_->key.size(), static_cast<std::size_t>(std::max(0, columns - 2)));
        return {" " + paint(tone::accent_bold, "输入 API 密钥 · " + login_->provider),
                "  " + std::string(visible, '*'),
                " " + paint(tone::dim, "Enter 保存  Esc 取消")};
    }

    if (selection_) {
        cursor_row = -1;
        cursor_col = 0;
        const auto capacity = static_cast<std::size_t>(std::max(1, size_.rows - 2));
        const auto visible = std::min({menu_rows, selection_->items.size(), capacity});
        const auto first = first_visible(selection_->selected_index, visible);
        rows = {" " + paint(tone::accent_bold, ui::one_line(selection_->title))};
        for (auto i = first; i < first + visible; ++i) {
            const bool selected = i == selection_->selected_index;
            auto label = ui::one_line(selection_->items[i]);
            if (i < selection_->details.size() && !selection_->details[i].empty()) {
                label += "  " + ui::one_line(selection_->details[i]);
            }
            rows.push_back(" " + paint(selected ? tone::accent_bold : tone::normal,
                                       (selected ? "→ " : "  ") + label));
        }
        rows.push_back(" " + paint(tone::dim, "↑↓ 选择  Enter 确认  Esc 取消"));
        return rows;
    }

    auto palette = palette_rows(columns);

    std::vector<std::string> file_completion;
    const auto files = file_items();
    if (!files.empty()) {
        const std::size_t show = std::min<std::size_t>(files.size(), 8);
        for (std::size_t i = 0; i < show; ++i) {
            const bool on = i == palette_index_ % files.size();
            auto name = files[i];
            if (text::width(name) > columns - 6) name = text::keep_tail(name, columns - 6);
            file_completion.push_back("  " + paint(on ? tone::accent : tone::dim, name));
        }
    }

    auto displayed = editor_.text();
    constexpr std::string_view separators = " \t\r\n";
    const auto command_end = displayed.find_first_of(separators);
    const bool login_draft = std::string_view(displayed).substr(0, command_end) == "/login";
    if (login_draft && command_end != std::string::npos) {
        const auto provider_start = displayed.find_first_not_of(separators, command_end);
        const auto provider_end = displayed.find_first_of(separators, provider_start);
        if (provider_end != std::string::npos) std::fill(displayed.begin() + provider_end + 1, displayed.end(), '*');
    }

    std::vector<std::string> hints;
    if (!login_draft && !editor_.text().empty() && !busy_ && !ask_) {
        auto validation = app_.validator.validate(editor_.text());
        if (validation.type != ode::input_validator::issue_type::none) {
            const auto icon = validation.is_warning ? "⚠️  " : "❌ ";
            const auto color = validation.is_warning ? tone::dim : tone::error;
            hints.push_back("  " + paint(color, icon + validation.message));
        } else if (palette.empty() && file_completion.empty() && editor_.text().size() > 3) {
            const auto suggestions = suggestion_items();
            if (!suggestions.empty()) {
                const std::size_t show = std::min<std::size_t>(suggestions.size(), 2);
                for (std::size_t i = 0; i < show; ++i) {
                    hints.push_back("  " + paint(tone::dim, suggestions[i]));
                }
            }
        }
    }

    const bool show_borders = size_.rows >= (ask_ ? 7 : 5);
    const bool show_status = show_borders || (!ask_ && size_.rows >= 3);
    const int chrome = (show_borders ? 2 : 0) + (show_status ? 1 : 0);
    const auto extra_rows = static_cast<std::size_t>(std::max(0, size_.rows - chrome - 2));
    if (ask_ && size_.rows > 1) {
        const auto head = std::string(ui::verb(ask_->name)) + " ";
        if (size_.rows > 2) {
            rows.push_back(" " + paint(tone::accent_bold, "? " + head) + paint(tone::bold, text::keep_head(ask_->label, columns - 6)));
        }
        rows.push_back("   " + paint(tone::accent, "⏎") + paint(tone::dim, " 允许   ") + paint(tone::accent, "esc") +
                       paint(tone::dim, " 拒绝   ") + paint(tone::accent, "a") +
                       paint(tone::dim, " 总是允许 " + text::keep_head(ask_->rule, std::max(8, columns - 32))));
    }
    if (!hint_.empty() && rows.size() < extra_rows) rows.push_back("  " + paint(tone::dim, hint_));
    for (auto* list : {&palette, &file_completion, &hints}) {
        for (auto& line : *list) {
            if (rows.size() >= extra_rows) break;
            rows.push_back(std::move(line));
        }
    }

    const auto l = editor_.arrange(std::max(1, columns - 4));
    const int max_input_lines = std::clamp(size_.rows - chrome - static_cast<int>(rows.size()) - 1, 1, 10);
    const int actual_input_lines = std::min(static_cast<int>(l.rows.size()), max_input_lines);
    const int first_input_line = std::max(0, l.cursor_row - actual_input_lines + 1);

    std::string border;
    if (show_borders) {
        border = app_.current_theme.border_fg + "\033[1m";
        for (int i = 0; i < columns; ++i) border += "─";
        border += "\033[0m";
        rows.push_back(border);
    }
    const auto input_start = static_cast<int>(rows.size());

    // Input lines
    for (int i = first_input_line; i < first_input_line + actual_input_lines; ++i) {
        const auto& r = l.rows[i];
        const auto piece = std::string_view(displayed).substr(r.begin, r.end - r.begin);
        std::string shown(piece);
        if (!shown.empty() && shown.back() == '\n') shown.pop_back();

        if (i == 0) {
            rows.push_back(app_.current_theme.prompt_fg + "\033[1m› \033[0m" +
                          app_.current_theme.prompt_fg + "\033[1m" + shown + "\033[0m");
        } else {
            rows.push_back(app_.current_theme.prompt_fg + "\033[1m  " + shown + "\033[0m");
        }
    }

    if (show_borders) rows.push_back(std::move(border));
    if (show_status) rows.push_back(status_line(columns));

    cursor_row = ask_ ? -1 : input_start + l.cursor_row - first_input_line;
    cursor_col = l.cursor_column + 2;  // +2 for "› " width

    return rows;
}

} // namespace ode
