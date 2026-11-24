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
};

const std::pair<const char*, const char*> builtin_commands[] = {
    {"/help", "快捷键与命令"},       {"/compact", "压缩上下文"},   {"/model", "查看或切换模型"},
    {"/mode", "切换权限档 plan/default/yolo"}, {"/resume", "恢复本目录的会话"}, {"/undo", "撤销上一轮的文件改动"},
    {"/clear", "开始新会话"},         {"/cost", "本会话 token 用量"}, {"/exit", "退出"},
};

std::string spinner(steady::time_point since) {
    // Unified dot rotation for all tools (Braille pattern)
    static constexpr const char* frames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧"};
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(steady::now() - since).count();
    return frames[(ms / 100) % 8];
}

std::string spinner_with_color(steady::time_point since, std::string_view tool_name) {
    // Color selection based on tool type
    const char* color = "\033[38;2;200;200;200m"; // Default gray

    if (tool_name == "Read") {
        color = "\033[38;2;79;193;255m";   // Blue
    } else if (tool_name == "Write") {
        color = "\033[38;2;206;145;120m";  // Orange
    } else if (tool_name == "Edit") {
        color = "\033[38;2;220;220;170m";  // Yellow
    } else if (tool_name == "Bash") {
        color = "\033[38;2;78;201;176m";   // Green
    } else if (tool_name == "Grep" || tool_name == "Glob") {
        color = "\033[38;2;78;201;176m";   // Cyan
    }

    return std::string(color) + spinner(since) + "\033[0m";
}

std::string seconds(steady::time_point since) {
    const auto s = std::chrono::duration_cast<std::chrono::seconds>(steady::now() - since).count();
    return s < 60 ? std::to_string(s) + "s" : std::to_string(s / 60) + "m" + std::to_string(s % 60) + "s";
}

} // namespace

std::vector<std::string> interactive_ui::palette_items() {
    std::vector<palette_item> items;
    const auto& t = editor_.text();

    // Only show palette if input starts with / and has no spaces or newlines
    if (t.empty() || t.front() != '/' || t.find_first_of(" \n") != std::string::npos) return {};

    // Add builtin commands
    for (const auto& [name, about] : builtin_commands) {
        if (std::string_view(name).starts_with(t)) {
            items.push_back({name, about, command_kind::builtin});
        }
    }

    // Add skills
    for (const auto& skill : app_.skills) {
        const auto name = "/" + skill.name;
        if (name.starts_with(t)) {
            items.push_back({name, skill.description + " (skill)", command_kind::skill});
        }
    }

    // Add user commands
    for (const auto& cmd : app_.commands) {
        const auto name = "/" + cmd.name;
        if (name.starts_with(t)) {
            items.push_back({name, cmd.description, command_kind::user_command});
        }
    }

    // Convert to string pairs (name, description)
    std::vector<std::string> result;
    for (const auto& item : items) {
        result.push_back(item.name + "\t" + item.description);
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
    const int columns = std::max(20, size_.columns - 1);

    // Unfinished streamed line.
    if (!stream_.empty()) {
        auto state = md_;
        for (auto& r : ui::markdown_line(stream_, state, columns)) rows.push_back(std::move(r));
    }
    for (const auto& t : running_) {
        const auto head = spinner_with_color(t.started, t.name) + " " + std::string(ui::verb(t.name)) + " ";
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
        rows.push_back(" " + paint(tone::accent, spinner_with_color(busy_since_, "Thinking")) + paint(tone::dim, " 思考中 " + seconds(busy_since_) + "  esc 中断"));
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

    // Fixed bottom input box design
    const int inner = columns - 4;
    const auto l = editor_.arrange(inner);

    // Calculate how many rows to reserve for input box and status
    const int max_input_lines = 10;  // Maximum input height
    const int actual_input_lines = std::min(static_cast<int>(l.rows.size()), max_input_lines);
    const int input_box_height = actual_input_lines + 2;  // +2 for top and bottom borders
    const int status_line_height = 1;
    const int reserved_bottom = input_box_height + status_line_height;

    // Calculate available space for content (ensure at least some space)
    const int min_content_rows = 5;  // Minimum rows for content
    const int available_rows = std::max(min_content_rows, size_.rows - reserved_bottom);

    // Only trim if we have too much content
    if (static_cast<int>(rows.size()) > available_rows) {
        // Keep the most recent content
        const int to_remove = static_cast<int>(rows.size()) - available_rows;
        rows.erase(rows.begin(), rows.begin() + to_remove);
    }

    // Don't add extra padding - let content flow naturally
    const int content_end = static_cast<int>(rows.size());

    // Draw input box with borders
    // Top border (can move up with more input lines)
    std::string top_border;
    for (int i = 0; i < columns; ++i) top_border += "─";
    rows.push_back(paint(tone::dim, top_border));

    // Input lines
    for (std::size_t i = 0; i < l.rows.size() && i < static_cast<std::size_t>(max_input_lines); ++i) {
        const auto& r = l.rows[i];
        const auto piece = std::string_view(editor_.text()).substr(r.begin, r.end - r.begin);
        std::string shown(piece);
        if (!shown.empty() && shown.back() == '\n') shown.pop_back();

        if (i == 0) {
            rows.push_back(paint(busy_ ? tone::dim : tone::accent, "› ") + shown);
        } else {
            rows.push_back("  " + shown);
        }
    }

    // Bottom border (fixed at bottom)
    std::string bottom_border;
    for (int i = 0; i < columns; ++i) bottom_border += "─";
    rows.push_back(paint(tone::dim, bottom_border));

    // Status line (fixed at very bottom)
    rows.push_back(status_line(columns));

    // Calculate cursor position
    cursor_row = ask_ ? -1 : content_end + 1 + l.cursor_row;  // +1 for top border
    cursor_col = l.cursor_column + 2;  // +2 for "› " width

    return rows;
}

} // namespace ode
