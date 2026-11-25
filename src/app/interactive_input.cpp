#include "app/interactive.hpp"
#include "ext/lua/host.hpp"

#include <iostream>

namespace ode {

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

    // Handle palette selection
    const auto items = palette_items();

    if (!items.empty() && text.front() == '/' && text.find_first_of(" \t\r\n") == std::string::npos) {
        if (palette_index_ < items.size()) {
            text = items[palette_index_].name;
        }
    }

    const auto name = std::string_view(text).substr(0, text.find_first_of(" \t\r\n"));
    const bool credentials = name == "/login" || name == "/logout";
    if (!credentials) {
        app_.history.add(text);
        editor_.remember(text);
    }

    editor_.clear();
    palette_index_ = 0;

    if (!text.empty() && text.front() == '/') {
        if (busy_) {
            hint_ = "模型工作中，命令需等这一轮结束";
            hint_at_ = steady::now();
            return;
        }
        // Try user commands first, then builtin
        if ((credentials || name == "/reload" || !user_command(text)) && !command(text)) quit_ = true;
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

void interactive_ui::handle(const ui::key& k, bool allow_lua) {
    if (login_) {
        handle_login(k);
        return;
    }
    // Selection mode takes priority
    if (selection_) {
        if (k.kind == key_kind::up) {
            if (selection_->selected_index > 0) selection_->selected_index--;
            else selection_->selected_index = selection_->items.size() - 1;  // Wrap to bottom
        } else if (k.kind == key_kind::down) {
            selection_->selected_index = (selection_->selected_index + 1) % selection_->items.size();
        } else if (k.kind == key_kind::enter) {
            auto callback = selection_->on_select;
            auto index = selection_->selected_index;
            selection_.reset();
            if (callback) callback(index);
        } else if (k.kind == key_kind::escape) {
            auto callback = selection_->on_cancel;
            selection_.reset();
            if (callback) callback();
        }
        return;
    }

    if (ask_) {
        if (k.kind == key_kind::enter || (k.kind == key_kind::character && (k.text == "y" || k.text == "Y"))) answer_ask(answer::yes);
        else if (k.kind == key_kind::escape || (k.kind == key_kind::character && (k.text == "n" || k.text == "N"))) answer_ask(answer::no);
        else if (k.kind == key_kind::character && (k.text == "a" || k.text == "A")) answer_ask(answer::always);
        else if (k.kind == key_kind::ctrl && k.letter == 'c') cancel();
        else if (k.kind == key_kind::shift_tab) app_.permissions.set_mode(next_mode(app_.permissions.mode()));
        return;
    }
    if (allow_lua && !busy_ && app_.lua && (k.kind == key_kind::ctrl || k.kind == key_kind::function_key)) {
        const auto name = k.kind == key_kind::ctrl
            ? "Ctrl+" + std::string(1, static_cast<char>('A' + k.letter - 'a'))
            : "F" + std::to_string(k.number);
        if (app_.lua->has_key(name)) {
            lua_key_ = k;
            lua_key_handled_ = false;
            start_job([this, name](agent_events&, cancel_token& cancel) {
                const auto result = app_.lua->run_key(name, cancel);
                lua_key_handled_ = result.handled;
                if (cancel.cancelled()) return turn_result{stop_reason::cancelled, "Cancelled."};
                if (!result.error.empty()) return turn_result{stop_reason::error, result.error};
                return turn_result{};
            }, false, false);
            return;
        }
    }
    const int inner = std::max(4, size_.columns - 7);
    const auto items = palette_items();
    const auto files = file_items();
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
            if (!items.empty()) palette_index_ = (palette_index_ + items.size() - 1) % items.size();
            else if (!files.empty()) palette_index_ = (palette_index_ + files.size() - 1) % files.size();
            else if (!editor_.vertical(-1, inner)) editor_.history_prev();
            break;
        case key_kind::down:
            if (!items.empty()) palette_index_ = (palette_index_ + 1) % items.size();
            else if (!files.empty()) palette_index_ = (palette_index_ + 1) % files.size();
            else if (!editor_.vertical(1, inner)) editor_.history_next();
            break;
        case key_kind::tab:
            if (!items.empty()) {
                if (palette_index_ < items.size()) editor_.set(items[palette_index_].name + " ");
            } else if (!files.empty()) {
                // Replace the @mention with the selected file
                const auto mention = editor_.at_mention();
                if (mention && palette_index_ < files.size()) {
                    const auto& selected = files[palette_index_];
                    // Delete backwards to the @, then insert @selected
                    for (std::size_t i = 0; i < mention->size(); ++i) editor_.backspace();
                    editor_.backspace();  // Remove the @
                    editor_.insert("@" + selected);
                }
            }
            break;
        case key_kind::shift_tab: {
            const auto m = next_mode(app_.permissions.mode());
            app_.permissions.set_mode(m);
            break;
        }
        case key_kind::escape:
            if (busy_) {
                cancel();
            } else if (editing_fork_) {
                // Cancel fork editing
                editing_fork_ = false;
                editor_.clear();
                note("已取消编辑", ui::tone::dim);
            } else {
                // Check for double-escape to fork
                const auto now = steady::now();
                if (last_escape_ != steady::time_point{} && now - last_escape_ < 500ms) {
                    // Double escape - fork the last user message
                    if (app_.core->messages.empty()) {
                        note("没有可编辑的消息", ui::tone::dim);
                    } else {
                        // Find the last user message
                        std::size_t user_idx = std::string::npos;
                        for (std::size_t i = app_.core->messages.size(); i-- > 0;) {
                            if (app_.core->messages[i].from == message::role::user) {
                                user_idx = i;
                                break;
                            }
                        }
                        if (user_idx == std::string::npos) {
                            note("没有可编辑的用户消息", ui::tone::dim);
                        } else {
                            // Enter fork editing mode
                            editing_fork_ = true;
                            const auto& last_user = app_.core->messages[user_idx];
                            // Extract text from blocks
                            std::string text;
                            for (const auto& block : last_user.blocks) {
                                if (block.type == block::kind::text) {
                                    text += block.text;
                                }
                            }
                            editor_.set(text);
                            hint_ = "编辑中 · Esc 取消 · Enter 创建分支";
                            hint_at_ = steady::now();
                        }
                    }
                    last_escape_ = {};
                } else {
                    if (!editor_.empty()) editor_.clear();
                    last_escape_ = now;
                }
            }
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
                case 'k': {
                    // Day 11: Kill to end of line (like Emacs/Bash)
                    while (editor_.cursor() < editor_.text().size()) {
                        if (editor_.text()[editor_.cursor()] == '\n') break;
                        editor_.erase_forward();
                    }
                    break;
                }
                case 'w': {
                    // Day 11: Delete word backward (like Emacs/Bash)
                    if (editor_.empty() || editor_.cursor() == 0) break;

                    // Skip trailing whitespace
                    while (editor_.cursor() > 0 &&
                           std::isspace(static_cast<unsigned char>(editor_.text()[editor_.cursor() - 1]))) {
                        editor_.backspace();
                    }

                    // Delete word characters
                    while (editor_.cursor() > 0 &&
                           !std::isspace(static_cast<unsigned char>(editor_.text()[editor_.cursor() - 1]))) {
                        editor_.backspace();
                    }
                    break;
                }
                case 'l': screen_.clear_screen(); break;
                case 't':
                    flush_thinking(true);
                    show_thinking_ = !show_thinking_;
                    hint_ = show_thinking_ ? "后续显示思考正文" : "后续只显示思考字数";
                    hint_at_ = steady::now();
                    break;
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

void interactive_ui::handle_login(const ui::key& k) {
    if (k.kind == key_kind::escape || (k.kind == key_kind::ctrl && k.letter == 'c')) {
        login_.reset();
    } else if (k.kind == key_kind::enter) {
        save_login();
    } else if (k.kind == key_kind::character) {
        login_->key += k.text;
    } else if (k.kind == key_kind::backspace && !login_->key.empty()) {
        login_->key.erase(text::prev_boundary(login_->key, login_->key.size()));
    }
}

} // namespace ode
