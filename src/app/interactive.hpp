#pragma once

#include "app/app.hpp"
#include "app/screen.hpp"
#include "ui/editor.hpp"
#include "ui/render.hpp"

#include <chrono>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <thread>
#include <variant>

namespace ode {

using steady = std::chrono::steady_clock;

// Events travel from the agent thread to the UI thread through this queue.
struct ev_text { std::string text; };
struct ev_thinking { std::string text; std::string signature; };
struct ev_request {};
struct ev_tool_start { std::string name, label; };
struct ev_progress { std::string text; };
struct ev_tool_done { std::string name, label, summary, detail, full; bool error = false; };
struct ev_ask { std::string name, label, rule; std::shared_ptr<std::promise<answer>> reply; };
struct ev_usage { std::size_t context = 0, input = 0, output = 0; };
struct ev_done { turn_result result; bool compact = false; };
using ui_event = std::variant<ev_text, ev_thinking, ev_request, ev_tool_start, ev_progress, ev_tool_done, ev_ask,
                              ev_usage, ev_done>;

struct running_tool {
    std::string name, label;
    steady::time_point started;
};

class interactive_ui {
public:
    interactive_ui(app& a, ui::terminal& term);
    ~interactive_ui();
    int run(std::string first_prompt);

    // Output helpers used by the slash commands.
    void blank();
    void note(std::string_view text, ui::tone tone = ui::tone::dim);
    void print_markdown(std::string_view text);
    bool command(const std::string& line);  // false: exit requested
    bool user_command(const std::string& line);  // false: not a user command

private:
    void handle(const ui::key& k);
    void submit();
    void start_turn(std::string text);
    void start_job(std::function<turn_result(agent_events&, cancel_token&)> job, bool compact);
    void drain();
    void apply(ev_text&);
    void apply(ev_thinking& e);
    void apply(ev_request&) { thinking_ = true; }
    void apply(ev_tool_start&);
    void apply(ev_progress&);
    void apply(ev_tool_done&);
    void apply(ev_ask&);
    void apply(ev_usage& u) { usage_ = u; }
    void apply(ev_done&);
    void section(std::string_view kind);
    void flush_stream(bool all);
    void answer_ask(answer a);
    void cancel();
    std::vector<std::string> live(int& cursor_row, int& cursor_col);
    std::vector<std::string> palette_rows(int columns);
    std::vector<std::string> palette_items();  // All available commands/skills/user commands
    std::vector<std::string> file_items();     // File completion candidates
    std::vector<std::string> suggestion_items();  // Day 8: Tool suggestions based on input
    std::string status_line(int columns) const;
    void push(ui_event e);
    agent_events make_events();
    void show_customize_menu();  // Show theme customization menu

    // Track recent permission requests to prevent loops
    struct recent_ask {
        std::string key;  // name + label
        steady::time_point when;
        answer response;
    };
    std::vector<recent_ask> recent_asks_;

    // Interactive selection state (for model selector, login, etc.)
    struct selection_state {
        std::vector<std::string> items;        // Item labels
        std::vector<std::string> details;      // Optional details for each item
        std::size_t selected_index = 0;        // Current selection
        std::string title;                     // Selection title
        std::function<void(std::size_t)> on_select;  // Callback when confirmed
        std::function<void()> on_cancel;       // Callback when cancelled
    };
    std::optional<selection_state> selection_;

    // 🆕 最近使用的命令（用于智能排序）
    std::deque<std::string> recent_commands_;  // 最近使用的命令，最新的在前
    static constexpr size_t MAX_RECENT_COMMANDS = 10;

    // 🆕 最近执行的命令（用于防止对话框突然消失）
    std::string last_executed_command_;
    steady::time_point last_executed_time_;

    app& app_;
    ui::terminal& term_;
    screen screen_;
    ui::editor editor_;
    ui::size size_;

    std::mutex mutex_;
    std::deque<ui_event> queue_;
    std::vector<std::string> steering_;  // guarded by mutex_
    std::thread worker_;
    cancel_token cancel_;

    bool busy_ = false;
    bool thinking_ = false;
    bool quit_ = false;
    bool editing_fork_ = false;         // True when editing a forked message
    steady::time_point last_escape_{};  // For double-escape detection
    steady::time_point ctrl_c_at_{};    // first Ctrl+C on an empty draft
    std::string hint_;                  // one-off line above the input box
    steady::time_point hint_at_{};
    ev_usage usage_;
    steady::time_point busy_since_;
    std::string stream_;               // assistant text not yet committed
    ui::markdown_state md_;
    std::vector<running_tool> running_;
    std::string progress_tail_;        // live command output
    std::optional<ev_ask> ask_;
    std::string last_tool_full_;       // for Ctrl+O
    std::string last_section_;         // "user", "assistant" or "tool"
    std::size_t palette_index_ = 0;
    std::string branch_;

    // Thinking block tracking
    std::vector<std::pair<std::string, bool>> thinking_blocks_;  // (signature, expanded)
    bool welcome_shown_ = false;
};

} // namespace ode
