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

const std::pair<const char*, const char*> builtin_commands[] = {
    {"/help", "📖 快捷键与命令"},
    // 🚀 快捷工作流命令
    {"/ref", "🔄 快速查看编辑 (Read→Edit→Write)"},
    {"/search", "🔍 搜索并查看 (Grep→Read)"},
    {"/s", "🔍 搜索快捷方式"},
    {"/fix", "🔧 智能检查修复"},
    {"/test", "🧪 运行测试 [别名: /t]"},
    {"/t", "🧪 测试快捷方式"},
    {"/build", "🏗️  构建项目 [别名: /b]"},
    {"/b", "🏗️  构建快捷方式"},
    {"/run", "▶️  运行程序 [别名: /r]"},
    {"/r", "▶️  运行快捷方式"},
    {"/commit", "💾 智能提交 [别名: /c]"},
    {"/c", "💾 提交快捷方式"},
    // 核心命令
    {"/compact", "📦 压缩上下文"},
    {"/model", "🤖 查看或切换模型"},
    {"/theme", "🎨 切换主题配色"},
    {"/mode", "🔐 切换权限档"},
    {"/resume", "⏮️  恢复会话"},
    {"/undo", "↩️  撤销改动"},
    {"/clear", "🆕 开始新会话"},
    {"/cost", "💰 查看用量"},
    {"/exit", "👋 退出"},
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

// 渲染进度条 [████████░░] 50%
std::string progress_bar(int percent, int width = 20) {
    if (percent < 0) return "";
    percent = std::clamp(percent, 0, 100);
    const int filled = (percent * width) / 100;
    std::string bar = "[";
    for (int i = 0; i < width; ++i) {
        bar += (i < filled) ? "█" : "░";
    }
    bar += "] " + std::to_string(percent) + "%";
    return bar;
}

// 格式化时间（毫秒 -> 可读字符串）
std::string format_ms(int64_t ms) {
    if (ms <= 0) return "";
    if (ms < 1000) return std::to_string(ms) + "ms";
    const auto s = ms / 1000;
    if (s < 60) return std::to_string(s) + "s";
    return std::to_string(s / 60) + "m" + std::to_string(s % 60) + "s";
}

// 格式化字节数
std::string format_bytes(size_t bytes) {
    if (bytes == 0) return "";
    if (bytes < 1024) return std::to_string(bytes) + "B";
    if (bytes < 1024 * 1024) return std::to_string(bytes / 1024) + "KB";
    return std::to_string(bytes / (1024 * 1024)) + "MB";
}

} // namespace

std::vector<std::string> interactive_ui::palette_items() {
    std::vector<palette_item> items;
    const auto& t = editor_.text();

    // Show palette if input starts with / (even with spaces - for fuzzy search)
    if (t.empty() || t.front() != '/') return {};

    // Extract search query (remove leading /)
    std::string query = t.substr(1);

    // Fuzzy match function (checks if all chars in query appear in target in order)
    auto fuzzy_match = [](std::string_view query, std::string_view target) -> int {
        if (query.empty()) return 100;  // Empty query matches everything

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

    // Format output with categories
    std::vector<std::string> result;
    command_kind last_kind = command_kind::builtin;

    for (const auto& item : items) {
        // Add category header when kind changes
        if (items.size() > 3 && item.kind != last_kind) {
            const char* category = item.kind == command_kind::skill ? "── Skills ──" :
                                  item.kind == command_kind::user_command ? "── User Commands ──" : "── Commands ──";
            result.push_back(std::string(category));
            last_kind = item.kind;
        }

        // Format: /name - description
        result.push_back(item.name + "\t" + item.description);

        // Limit to avoid overwhelming display
        if (result.size() >= 15) break;
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

    for (std::size_t i = 0; i < items.size() && i < 8; ++i) {
        const bool on = i == palette_index_;
        // Split by tab
        const auto tab = items[i].find('\t');
        const auto name = items[i].substr(0, tab);
        const auto desc = tab != std::string::npos ? items[i].substr(tab + 1) : std::string{};

        auto padded_name = name;
        padded_name.resize(std::max<std::size_t>(padded_name.size(), 10), ' ');
        auto row = "  " + paint(on ? tone::accent_bold : tone::normal, padded_name) + "  " + paint(tone::dim, desc);
        rows.push_back(text::width(padded_name) + 4 > columns ? text::keep_head(row, columns) : row);
    }
    return rows;
}

std::string interactive_ui::status_line(int columns) const {
    const auto mode = app_.core ? app_.config.permissions.mode() : permission_mode::normal;
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

    // Unfinished streamed line.
    if (!stream_.empty()) {
        auto state = md_;
        for (auto& r : ui::markdown_line(stream_, state, columns)) rows.push_back(std::move(r));
    }
    for (const auto& t : running_) {
        const auto head = spinner_with_color(t.started, t.name, app_.current_theme) + " " + std::string(ui::verb(t.name)) + " ";

        // 有详细进度信息
        if (t.percent >= 0) {
            // 第一行：spinner + 名称 + 标签 + 进度条 + 时间
            const auto pbar = progress_bar(t.percent, 10);  // 紧凑的 10 字符进度条
            const auto elapsed = format_ms(t.elapsed_ms);
            const auto estimated = t.estimated_ms > 0 ? "~" + format_ms(t.estimated_ms) : "";
            const auto timing = elapsed + (estimated.empty() ? "" : "/" + estimated);

            // 计算剩余空间
            const int fixed_width = 4 + text::width(head) + text::width(pbar) + text::width(timing) + 4;
            const auto label = text::keep_head(t.label, std::max(8, columns - fixed_width));

            rows.push_back(" " + paint(tone::accent, head) + label + "  " +
                          paint(tone::accent, pbar) + "  " + paint(tone::dim, timing));

            // 第二行：当前步骤（如果有）
            if (!t.current_step.empty()) {
                const auto step_text = "  " + t.current_step;
                rows.push_back("   " + paint(tone::dim, "│ " + text::keep_head(step_text, columns - 6)));
            }

            // 第三行：资源信息（如果显著）
            if (t.bytes_io > 1024 * 100 || t.memory_kb > 1024) {  // 超过 100KB IO 或 1MB 内存才显示
                std::string resources;
                if (t.bytes_io > 0) resources += format_bytes(t.bytes_io) + " IO";
                if (t.memory_kb > 1024) {
                    if (!resources.empty()) resources += "  ";
                    resources += format_bytes(t.memory_kb * 1024) + " mem";
                }
                if (!resources.empty()) {
                    rows.push_back("   " + paint(tone::dim, "│ " + resources));
                }
            }
        } else {
            // 传统显示（无进度信息）
            const auto tail = "  " + seconds(t.started);
            const auto label = text::keep_head(t.label, std::max(8, columns - 4 - text::width(head) - text::width(tail)));
            rows.push_back(" " + paint(tone::accent, head) + label + paint(tone::dim, tail));
        }
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

            std::string line = "  " + std::to_string(i + 1) + ". " + selection_->items[i];
            if (i < selection_->details.size() && !selection_->details[i].empty()) {
                line += "  " + selection_->details[i];
            }
            if (is_selected) {
                line += " ←";  // Arrow on the right for selected item
            }

            rows.push_back(" " + paint(item_color, line));
        }

        rows.push_back("");
        rows.push_back(" " + paint(tone::dim, "Use ↑↓ to select, Enter to confirm, Esc to cancel"));
    }

    for (auto& r : palette_rows(columns)) rows.push_back(std::move(r));

    // File completion palette
    const auto files = file_items();
    if (!files.empty()) {
        const std::size_t show = std::min<std::size_t>(files.size(), 8);
        for (std::size_t i = 0; i < show; ++i) {
            const bool on = i == palette_index_ % files.size();
            auto name = files[i];
            if (text::width(name) > columns - 6) name = text::keep_tail(name, columns - 6);
            rows.push_back("  " + paint(on ? tone::accent : tone::dim, name));
        }
    }

    // Smart hints with priority: validation > suggestions
    // Only show one type at a time to avoid clutter
    if (!editor_.text().empty() && !busy_ && !ask_) {
        // Priority 1: Input validation (errors/warnings)
        auto validation = app_.validator.validate(editor_.text());
        if (validation.type != ode::input_validator::issue_type::none) {
            const auto icon = validation.is_warning ? "⚠️  " : "❌ ";
            const auto color = validation.is_warning ? tone::dim : tone::error;
            rows.push_back("  " + paint(color, icon + validation.message));
        }
        // Priority 2: Tool suggestions (only if no validation issues and no other completions)
        else if (palette_rows(columns).empty() && files.empty() && editor_.text().size() > 3) {
            const auto suggestions = suggestion_items();
            if (!suggestions.empty()) {
                // Max 2 suggestions (not 3) for cleaner display
                const std::size_t show = std::min<std::size_t>(suggestions.size(), 2);
                for (std::size_t i = 0; i < show; ++i) {
                    rows.push_back("  " + paint(tone::dim, suggestions[i]));
                }
            }
        }
    }

    // Fixed bottom input box design
    const int inner = columns - 4;
    const auto l = editor_.arrange(inner);

    // Calculate how many rows to reserve for input box and status
    const int max_input_lines = 10;  // Maximum input height
    const int actual_input_lines = std::min(static_cast<int>(l.rows.size()), max_input_lines);
    const int input_box_height = actual_input_lines + 2;  // +2 for top and bottom borders
    const int status_line_height = 1;
    const int reserved_bottom = input_box_height + status_line_height;

    // Calculate available space for content
    const int available_rows = size_.rows - reserved_bottom;

    // Trim or pad content to fit exactly in available space
    if (static_cast<int>(rows.size()) > available_rows) {
        // Too much content - remove from top
        const int to_remove = static_cast<int>(rows.size()) - available_rows;
        rows.erase(rows.begin(), rows.begin() + to_remove);
    } else {
        // Not enough content - add padding to push input to bottom
        while (static_cast<int>(rows.size()) < available_rows) {
            rows.push_back("");
        }
    }

    const int content_end = static_cast<int>(rows.size());

    // Draw input box with borders (now guaranteed at bottom)
    // Top border - use cached theme color with BOLD
    std::string top_border;
    top_border.reserve(columns + 20);  // Preallocate
    for (int i = 0; i < columns; ++i) top_border += "─";
    rows.push_back(app_.current_theme.border_fg + "\033[1m" + top_border + "\033[0m");

    // Input lines
    for (std::size_t i = 0; i < l.rows.size() && i < static_cast<std::size_t>(max_input_lines); ++i) {
        const auto& r = l.rows[i];
        const auto piece = std::string_view(editor_.text()).substr(r.begin, r.end - r.begin);
        std::string shown(piece);
        if (!shown.empty() && shown.back() == '\n') shown.pop_back();

        if (i == 0) {
            // Use cached theme color for input prompt - BOLD
            rows.push_back(app_.current_theme.prompt_fg + "\033[1m› \033[0m" +
                          app_.current_theme.prompt_fg + "\033[1m" + shown + "\033[0m");
        } else {
            // Continuation lines also bold
            rows.push_back(app_.current_theme.prompt_fg + "\033[1m  " + shown + "\033[0m");
        }
    }

    // Bottom border - use cached color with BOLD
    std::string bottom_border;
    bottom_border.reserve(columns + 20);  // Preallocate
    for (int i = 0; i < columns; ++i) bottom_border += "─";
    rows.push_back(app_.current_theme.border_fg + "\033[1m" + bottom_border + "\033[0m");

    // Status line (fixed at very bottom)
    rows.push_back(status_line(columns));

    // Calculate cursor position
    cursor_row = ask_ || selection_ ? -1 : content_end + 1 + l.cursor_row;  // +1 for top border
    cursor_col = l.cursor_column + 2;  // +2 for "› " width

    return rows;
}

} // namespace ode
