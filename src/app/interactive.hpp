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
struct ev_thinking { std::string text; };
struct ev_request {};
struct ev_tool_preparing { std::string name; steady::time_point started; };
struct ev_tool_start { std::string name, label, command; steady::time_point started; };
struct ev_progress { std::string text; };
struct ev_tool_done {
    std::string name, label, summary, detail, full;
    bool error = false;
    std::optional<std::chrono::milliseconds> elapsed;
};
struct ev_ask { std::string name, label, rule; std::shared_ptr<std::promise<answer>> reply; };
struct ev_usage { std::size_t context = 0, input = 0, output = 0; };
struct ev_notice { std::string message; std::optional<std::size_t> context; };
struct ev_done { turn_result result; bool compact = false; };
using ui_event = std::variant<ev_text, ev_thinking, ev_request, ev_tool_preparing, ev_tool_start, ev_progress,
                              ev_tool_done, ev_ask, ev_usage, ev_notice, ev_done>;

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
    void handle(const ui::key& k, bool allow_lua = true);
    void submit();
    void handle_login(const ui::key& k);
    void login_command(std::string_view arg);
    void logout_command(std::string_view arg);
    bool switch_to_saved_provider(const std::string& name);
    void save_login();
    void start_turn(std::string text, std::vector<std::string> allowed_tools = {});
    void start_job(std::function<turn_result(agent_events&, cancel_token&)> job, bool compact, bool needs_provider = true);
    void drain();
    void apply(ev_text&);
    void apply(ev_thinking& e);
    void apply(ev_request&);
    void apply(ev_tool_preparing& e);
    void apply(ev_tool_start&);
    void apply(ev_progress&);
    void apply(ev_tool_done&);
    void apply(ev_ask&);
    void apply(ev_usage& u) { usage_ = u; }
    void apply(ev_notice&);
    void apply(ev_done&);
    void section(std::string_view kind);
    void flush_stream(bool all);
    void flush_thinking(bool all);
    void flush_command_output(bool all);
    void show_shell_command(std::string_view command);
    std::vector<std::string> tool_detail_rows(std::string_view line, ui::tone tone) const;
    std::vector<std::string> thinking_rows(std::string_view line, int columns) const;
    void answer_ask(answer a);
    void cancel();
    std::vector<std::string> activity_rows() const;
    std::vector<std::string> footer_rows(int& cursor_row, int& cursor_col);
    void show_session_history();
    void show_user_message(std::string_view text);
    std::vector<std::string> palette_rows(int columns, std::size_t capacity);
    enum class command_kind { builtin, skill, user_command };
    struct palette_item {
        std::string name;
        std::string description;
        command_kind kind;
        int score;
    };
    std::vector<palette_item> palette_items() const;
    std::vector<std::string> file_items();     // File completion candidates
    std::vector<std::string> suggestion_items();  // Day 8: Tool suggestions based on input
    std::string status_line(int columns) const;
    static std::string format_elapsed(std::chrono::milliseconds elapsed);
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
    struct login_state {
        std::string provider;
        std::string key;
    };
    std::optional<login_state> login_;
    std::optional<ui::key> lua_key_;
    bool lua_key_handled_ = false;

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
    std::string think_stream_;
    std::size_t hidden_thinking_chars_ = 0;
    bool show_thinking_ = true;
    bool think_started_ = false;
    ui::markdown_state md_;
    std::vector<running_tool> running_;
    std::optional<ev_tool_preparing> preparing_tool_;
    std::string progress_tail_;
    std::size_t command_output_bytes_ = 0;
    bool command_output_truncated_ = false;
    std::optional<ev_ask> ask_;
    std::string last_tool_full_;       // for Ctrl+O
    std::string last_section_;         // "user", "assistant" or "tool"
    std::size_t palette_index_ = 0;
    std::string branch_;

    bool welcome_shown_ = false;
};

} // namespace ode
