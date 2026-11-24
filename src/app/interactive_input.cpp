#include "app/interactive.hpp"

namespace lay {

using namespace std::chrono_literals;
using ui::key_kind;

void interactive_ui::cancel() {
    cancel_.cancel();
    if (ask_) answer_ask(answer::no);
}

void interactive_ui::submit() {
    auto text = editor_.text();
    while (!text.empty() && (text.back() == '\n' || text.back() == ' ')) text.pop_back();
    if (text.empty()) return;
    // A command chosen in the palette runs as typed in full.
    const auto matches = palette_matches();
    if (!matches.empty() && text != matches[palette_index_ % matches.size()].first)
        text = matches[palette_index_ % matches.size()].first;
    editor_.remember(text);
    editor_.clear();
    palette_index_ = 0;
    if (text.front() == '/') {
        if (busy_) {
            hint_ = "模型工作中，命令需等这一轮结束";
            hint_at_ = steady::now();
            return;
        }
        if (!command(text)) quit_ = true;
        return;
    }
    if (busy_) {
        // Steering: delivered with the next tool results, or as the next turn.
        std::lock_guard lock(mutex_);
        steering_.push_back(text);
        section("user");
        screen_.commit(ui::layout({{text, ui::tone::dim}}, size_.columns - 1, {ui::paint(ui::tone::dim, "› "), 2}, {"  ", 2}));
        return;
    }
    start_turn(std::move(text));
}

void interactive_ui::handle(const ui::key& k) {
    if (ask_) {
        if (k.kind == key_kind::enter || (k.kind == key_kind::character && (k.text == "y" || k.text == "Y"))) answer_ask(answer::yes);
        else if (k.kind == key_kind::escape || (k.kind == key_kind::character && (k.text == "n" || k.text == "N"))) answer_ask(answer::no);
        else if (k.kind == key_kind::character && (k.text == "a" || k.text == "A")) answer_ask(answer::always);
        else if (k.kind == key_kind::ctrl && k.letter == 'c') cancel();
        else if (k.kind == key_kind::shift_tab) app_.config.permissions.set_mode(next_mode(app_.config.permissions.mode()));
        return;
    }
    const int inner = std::max(4, size_.columns - 7);
    const auto matches = palette_matches();
    switch (k.kind) {
        case key_kind::character: editor_.insert(k.text); palette_index_ = 0; break;
        case key_kind::newline: editor_.insert("\n"); break;
        case key_kind::enter:
            // A trailing backslash continues the draft on a new line.
            if (!editor_.empty() && editor_.text().back() == '\\' && editor_.cursor() == editor_.text().size()) {
                editor_.backspace();
                editor_.insert("\n");
            } else {
                submit();
            }
            break;
        case key_kind::backspace: editor_.backspace(); palette_index_ = 0; break;
        case key_kind::del: editor_.erase_forward(); break;
        case key_kind::left: editor_.left(); break;
        case key_kind::right: editor_.right(); break;
        case key_kind::word_left: editor_.word_left(); break;
        case key_kind::word_right: editor_.word_right(); break;
        case key_kind::home: editor_.home(); break;
        case key_kind::end: editor_.end(); break;
        case key_kind::up:
            if (!matches.empty()) palette_index_ = (palette_index_ + matches.size() - 1) % matches.size();
            else if (!editor_.vertical(-1, inner)) editor_.history_prev();
            break;
        case key_kind::down:
            if (!matches.empty()) palette_index_ = (palette_index_ + 1) % matches.size();
            else if (!editor_.vertical(1, inner)) editor_.history_next();
            break;
        case key_kind::tab:
            if (!matches.empty()) editor_.set(matches[palette_index_ % matches.size()].first + " ");
            break;
        case key_kind::shift_tab: {
            const auto m = next_mode(app_.config.permissions.mode());
            app_.config.permissions.set_mode(m);
            break;
        }
        case key_kind::escape:
            if (busy_) cancel();
            else editor_.clear();
            break;
        case key_kind::ctrl:
            switch (k.letter) {
                case 'c':
                    if (busy_) {
                        cancel();
                    } else if (!editor_.empty()) {
                        editor_.clear();
                    } else if (ctrl_c_at_ != steady::time_point{} && steady::now() - ctrl_c_at_ < 2s) {
                        quit_ = true;
                    } else {
                        ctrl_c_at_ = hint_at_ = steady::now();
                        hint_ = "再按一次 Ctrl+C 退出";
                    }
                    break;
                case 'd': if (editor_.empty() && !busy_) quit_ = true; else editor_.erase_forward(); break;
                case 'a': editor_.home(); break;
                case 'e': editor_.end(); break;
                case 'u': editor_.kill_line_before(); break;
                case 'l': screen_.clear_screen(); break;
                case 'o':
                    if (!last_tool_full_.empty()) {
                        auto shown = clip(last_tool_full_, 12000);
                        for (auto& row : text::wrap(shown, size_.columns - 6))
                            screen_.commit("   " + ui::paint(ui::tone::dim, "│ " + row));
                        last_tool_full_.clear();
                    }
                    break;
                default: break;
            }
            break;
        default: break;
    }
}

} // namespace lay
