#include "app/interactive.hpp"

namespace ode {

using ui::paint;
using ui::tone;

namespace {

enum class command_kind { builtin, skill, user_command };

struct palette_item {
    std::string name;
    std::string description;
    command_kind kind;
    int score = 0;  // For fuzzy search ranking
};

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
    {"/format", "代码格式化"},
    {"/doc", "生成文档"},
    {"/goal", "设置工作目标"},
    {"/compact", "压缩上下文"},
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

std::vector<std::string> interactive_ui::palette_items() {
    std::vector<palette_item> items;
    const auto& t = editor_.text();

    // 🔧 修复：防止对话框突然消失
    // 策略：在命令执行期间（busy_ 时），继续显示上次的命令
    bool just_executed = false;
    if (!last_executed_command_.empty()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            steady::now() - last_executed_time_).count();
        // 在执行后 1000ms 内，或者在 busy 状态时，继续显示
        if (elapsed < 1000 || busy_) {
            just_executed = true;
        } else {
            last_executed_command_.clear();  // 清除过期的命令
        }
    }

    // Show palette if input starts with / OR just executed a command OR busy
    if (!just_executed && (t.empty() || t.front() != '/')) return {};

    // Extract search query (remove leading /)
    std::string query = just_executed ? last_executed_command_.substr(1) : t.substr(1);

    // Fuzzy match function (checks if all chars in query appear in target in order)
    auto fuzzy_match = [](std::string_view query, std::string_view target) -> int {
        if (query.empty()) return 100;  // Empty query matches everything

        // Boost exact prefix match significantly
        if (target.starts_with(query)) return 1000;

        size_t qi = 0, ti = 0;
        int score = 0;
        while (qi < query.size() && ti < target.size()) {
            if (std::tolower(query[qi]) == std::tolower(target[ti])) {
                score += 10;
                if (ti == 0 || target[ti-1] == ' ' || target[ti-1] == '/') {
                    score += 5;  // Bonus for word start
                }
                qi++;
            }
            ti++;
        }
        return qi == query.size() ? score : -1;  // -1 = no match
    };

    // Add builtin commands with fuzzy matching
    for (const auto& [name, about] : builtin_commands) {
        int score = fuzzy_match(query, name);
        if (score >= 0) {
            // 🆕 检查是否在最近使用中，提升分数
            auto recent_it = std::find(recent_commands_.begin(), recent_commands_.end(), name);
            if (recent_it != recent_commands_.end()) {
                size_t recent_index = std::distance(recent_commands_.begin(), recent_it);
                score += (10 - recent_index) * 5;  // 最近的加分更多
            }
            items.push_back({name, about, command_kind::builtin, score});
        }
    }

    // Add skills with fuzzy matching
    for (const auto& skill : app_.skills) {
        const auto name = "/" + skill.name;
        int score = fuzzy_match(query, name);
        if (score >= 0) {
            items.push_back({name, skill.description + " (skill)", command_kind::skill, score});
        }
    }

    // Add user commands with fuzzy matching
    for (const auto& cmd : app_.commands) {
        const auto name = "/" + cmd.name;
        int score = fuzzy_match(query, name);
        if (score >= 0) {
            items.push_back({name, cmd.description, command_kind::user_command, score});
        }
    }

    // Sort by score (highest first), then by kind, then by name
    std::sort(items.begin(), items.end(), [](const auto& a, const auto& b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.kind != b.kind) return a.kind < b.kind;
        return a.name < b.name;
    });

    // Format output: each row is "name\tdescription [shortcut]"
    std::vector<std::string> result;
    for (const auto& item : items) {
        std::string shortcut;
        for (const auto& [cmd, key] : command_shortcuts) {
            if (cmd == item.name) {
                shortcut = key;
                break;
            }
        }

        std::string line = item.name + "\t" + item.description;
        if (!shortcut.empty()) line += " [" + shortcut + "]";
        result.push_back(line);
    }

    return result;
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
    if (items.empty()) return rows;
    if (palette_index_ >= items.size()) palette_index_ = 0;

    // 🎨 Pi 风格：固定显示数量，滚动窗口
    constexpr size_t MAX_VISIBLE = 6;  // 固定显示 6 个命令

    // 计算滚动窗口的起始位置
    size_t start_index = 0;
    if (items.size() > MAX_VISIBLE && palette_index_ >= MAX_VISIBLE - 1) {
        // 当选中项在第 6 个或之后，窗口开始滚动
        // 保持选中项在窗口底部
        start_index = palette_index_ - (MAX_VISIBLE - 1);
        // 但不要超出列表末尾
        if (start_index + MAX_VISIBLE > items.size()) {
            start_index = items.size() - MAX_VISIBLE;
        }
    }

    size_t visible_count = std::min(MAX_VISIBLE, items.size() - start_index);

    // 渲染可见的命令
    for (std::size_t i = 0; i < visible_count; ++i) {
        const std::size_t item_index = start_index + i;
        const bool on = item_index == palette_index_;

        // Split by tab
        const auto tab = items[item_index].find('\t');
        const auto name = items[item_index].substr(0, tab);
        const auto desc = tab != std::string::npos ? items[item_index].substr(tab + 1) : std::string{};

        // Pi 风格：选中项用 ▶ 标记
        std::string prefix = on ? "▶ " : "  ";

        auto padded_name = name;
        padded_name.resize(std::max<std::size_t>(padded_name.size(), 10), ' ');
        auto row = prefix + paint(on ? tone::accent_bold : tone::normal, padded_name) + "  " + paint(tone::dim, desc);
        rows.push_back(text::width(padded_name) + 4 > columns ? text::keep_head(row, columns) : row);
    }

    // 显示滚动指示器（有更多内容时）
    if (items.size() > MAX_VISIBLE) {
        std::string indicator = "  " + paint(tone::dim,
            "[" + std::to_string(palette_index_ + 1) + "/" + std::to_string(items.size()) + "]");
        rows.push_back(indicator);
    }

    return rows;
}

std::string interactive_ui::status_line(int columns) const {
    const auto mode = app_.core ? app_.permissions.mode() : permission_mode::normal;
    std::string right = std::string(mode_name(mode)) + " · " + app_.core->model;
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
    left = room > 8 ? text::keep_tail(left, room) : std::string{};
    const int gap = std::max(1, columns - 2 - text::width(left) - text::width(right));
    const auto mode_text = std::string(mode_name(mode));
    return "  " + paint(tone::dim, left) + std::string(static_cast<std::size_t>(gap), ' ') + paint(mode_tone, mode_text) +
           paint(tone::dim, right.substr(mode_text.size()));
}

std::vector<std::string> interactive_ui::live(int& cursor_row, int& cursor_col) {
    std::vector<std::string> rows;
    rows.reserve(size_.rows);  // Preallocate for performance
    const int columns = std::max(20, size_.columns - 1);

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
    if (ask_) {
        const auto head = std::string(ui::verb(ask_->name)) + " ";
        rows.push_back(" " + paint(tone::accent_bold, "? " + head) + paint(tone::bold, text::keep_head(ask_->label, columns - 6)));
        rows.push_back("   " + paint(tone::accent, "⏎") + paint(tone::dim, " 允许   ") + paint(tone::accent, "esc") +
                       paint(tone::dim, " 拒绝   ") + paint(tone::accent, "a") +
                       paint(tone::dim, " 总是允许 " + text::keep_head(ask_->rule, std::max(8, columns - 32))));
    } else if (busy_ && running_.empty() && (thinking_ || stream_.empty())) {
        rows.push_back(" " + paint(tone::accent, spinner_with_color(busy_since_, "Thinking", app_.current_theme)) + paint(tone::dim, " 思考中 " + seconds(busy_since_) + "  esc 中断"));
    }
    if (!hint_.empty()) rows.push_back("  " + paint(tone::dim, hint_));

    // Selection mode UI
    if (selection_) {
        rows.push_back("");  // Blank line
        rows.push_back(" " + paint(tone::accent_bold, selection_->title));
        rows.push_back("");

        for (std::size_t i = 0; i < selection_->items.size(); ++i) {
            const bool is_selected = i == selection_->selected_index;
            const auto item_color = is_selected ? tone::accent_bold : tone::normal;

            std::string line = std::to_string(i + 1) + ". " + selection_->items[i];
            if (i < selection_->details.size() && !selection_->details[i].empty()) {
                line += "  " + selection_->details[i];
            }

            std::string prefix = is_selected ? "→ " : "  ";
            rows.push_back(" " + paint(item_color, prefix + line));
        }

        rows.push_back("");
        rows.push_back(" " + paint(tone::dim, "Use ↑↓ to select, Enter to confirm, Esc to cancel"));
    }

    // Prepare bottom UI elements (but don't add to rows yet)
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

    std::vector<std::string> hints;
    if (!editor_.text().empty() && !busy_ && !ask_) {
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

    // Calculate input box dimensions
    const int inner = columns - 4;
    const auto l = editor_.arrange(inner);
    const int max_input_lines = 10;
    const int actual_input_lines = std::min(static_cast<int>(l.rows.size()), max_input_lines);
    const int input_box_height = actual_input_lines + 2;  // +2 for borders

    // Reserve space for: input box + palette + file completion + hints + status
    const int reserved_bottom = input_box_height +
                               static_cast<int>(palette.size()) +
                               static_cast<int>(file_completion.size()) +
                               static_cast<int>(hints.size()) +
                               1;  // status line

    // Calculate available space for content and trim if needed
    const int available_rows = size_.rows - reserved_bottom;
    if (static_cast<int>(rows.size()) > available_rows) {
        const int to_remove = static_cast<int>(rows.size()) - std::max(0, available_rows);
        rows.erase(rows.begin(), rows.begin() + to_remove);
    }

    const int content_end = static_cast<int>(rows.size());

    // Draw input box with borders
    std::string top_border;
    top_border.reserve(columns + 20);
    for (int i = 0; i < columns; ++i) top_border += "─";
    rows.push_back(app_.current_theme.border_fg + "\033[1m" + top_border + "\033[0m");

    // Input lines
    for (std::size_t i = 0; i < l.rows.size() && i < static_cast<std::size_t>(max_input_lines); ++i) {
        const auto& r = l.rows[i];
        const auto piece = std::string_view(editor_.text()).substr(r.begin, r.end - r.begin);
        std::string shown(piece);
        if (!shown.empty() && shown.back() == '\n') shown.pop_back();

        if (i == 0) {
            rows.push_back(app_.current_theme.prompt_fg + "\033[1m› \033[0m" +
                          app_.current_theme.prompt_fg + "\033[1m" + shown + "\033[0m");
        } else {
            rows.push_back(app_.current_theme.prompt_fg + "\033[1m  " + shown + "\033[0m");
        }
    }

    // Bottom border
    std::string bottom_border;
    bottom_border.reserve(columns + 20);
    for (int i = 0; i < columns; ++i) bottom_border += "─";
    rows.push_back(app_.current_theme.border_fg + "\033[1m" + bottom_border + "\033[0m");

    // Now add palette, file completion, and hints below the input box
    for (auto& r : palette) rows.push_back(std::move(r));
    for (auto& r : file_completion) rows.push_back(std::move(r));
    for (auto& r : hints) rows.push_back(std::move(r));

    // Status line at very bottom
    rows.push_back(status_line(columns));

    // Calculate cursor position (input box is now at content_end)
    cursor_row = ask_ || selection_ ? -1 : content_end + 1 + l.cursor_row;  // +1 for top border
    cursor_col = l.cursor_column + 2;  // +2 for "› " width

    return rows;
}

} // namespace ode
