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
    static constexpr const char* frames[] = {"⠋", "⠙", "⠹", "⠸", "⠼", "⠴", "⠦", "⠧", "⠇", "⠏"};
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(steady::now() - since).count();
    return frames[(ms / 100) % 10];
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
        const auto head = spinner(t.started) + " " + std::string(ui::verb(t.name)) + " ";
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
        rows.push_back(" " + paint(tone::accent, spinner(busy_since_)) + paint(tone::dim, " 思考中 " + seconds(busy_since_) + "  esc 中断"));
    }
    if (!hint_.empty()) rows.push_back("  " + paint(tone::dim, hint_));
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

    // Input box: ╭─╮ │ › text │ ╰─╯
    const int inner = std::max(4, columns - 6);
    std::string bar;
    for (int i = 0; i < columns - 2; ++i) bar += "─";
    const auto border = ask_ ? tone::dim : busy_ ? tone::dim : tone::accent;
    rows.push_back(paint(border, "╭" + bar + "╮"));
    const auto l = editor_.arrange(inner);
    const int first_input = static_cast<int>(rows.size());
    for (std::size_t i = 0; i < l.rows.size(); ++i) {
        const auto& r = l.rows[i];
        const auto piece = std::string_view(editor_.text()).substr(r.begin, r.end - r.begin);
        std::string shown(piece);
        if (!shown.empty() && shown.back() == '\n') shown.pop_back();
        const int pad = std::max(0, inner - text::width(shown));
        const auto mark = i == 0 ? paint(busy_ ? tone::dim : tone::accent, "› ") : std::string("  ");
        rows.push_back(paint(border, "│") + " " + mark + shown + std::string(static_cast<std::size_t>(pad), ' ') + paint(border, "│"));
    }
    rows.push_back(paint(border, "╰" + bar + "╯"));
    rows.push_back(status_line(columns));
    cursor_row = ask_ ? -1 : first_input + l.cursor_row;
    cursor_col = 4 + l.cursor_column;
    return rows;
}

} // namespace ode
