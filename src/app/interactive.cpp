#include "app/interactive.hpp"
#include "ui/welcome.hpp"

namespace ode {

using namespace std::chrono_literals;

interactive_ui::interactive_ui(app& a, ui::terminal& term) : app_(a), term_(term), screen_(term) {
    size_ = term_.dimensions();
    branch_ = git_branch(app_.root);

    // Day 10: Load command history
    const auto history_path = app_.root / ".claude" / "command_history.txt";
    if (fs::exists(history_path)) {
        app_.history.load(history_path);
    }
}

interactive_ui::~interactive_ui() {
    // Day 10: Save command history
    const auto history_path = app_.root / ".claude" / "command_history.txt";
    try {
        fs::create_directories(history_path.parent_path());
        app_.history.save(history_path);
    } catch (...) {
        // Ignore save errors
    }

    cancel_.cancel();
    // The worker may be blocked on a confirmation; answer it so the thread can finish.
    while (worker_.joinable()) {
        if (ask_) answer_ask(answer::no);
        std::deque<ui_event> events;
        {
            std::lock_guard lock(mutex_);
            events.swap(queue_);
        }
        bool done = false;
        for (auto& e : events) {
            if (auto* ask = std::get_if<ev_ask>(&e)) ask->reply->set_value(answer::no);
            if (std::holds_alternative<ev_done>(e)) done = true;
        }
        if (done) worker_.join();
        else std::this_thread::sleep_for(10ms);
    }
}

void interactive_ui::push(ui_event e) {
    std::lock_guard lock(mutex_);
    queue_.push_back(std::move(e));
}

agent_events interactive_ui::make_events() {
    agent_events ev;
    ev.request_started = [this] { push(ev_request{}); };
    ev.text = [this](std::string_view t) { push(ev_text{std::string(t)}); };
    ev.thinking = [this](std::string_view text, std::string_view) {
        if (!text.empty()) push(ev_thinking{std::string(text)});
    };
    ev.tool_started = [this](const tool& t, const json& in) { push(ev_tool_start{t.name, tool_label(t, in, app_.root)}); };
    ev.tool_progress = [this](std::string_view s) { push(ev_progress{std::string(s)}); };
    ev.tool_finished = [this](const tool& t, const json& in, const tool_output& o) {
        push(ev_tool_done{t.name, tool_label(t, in, app_.root), o.summary, o.detail, o.text, o.is_error});
    };
    ev.ask = [this](const tool& t, const json& in) {
        auto promise = std::make_shared<std::promise<answer>>();
        auto future = promise->get_future();
        push(ev_ask{t.name, tool_label(t, in, app_.root), permission_rules::always_rule(t, in), promise});
        return future.get();
    };
    // Runs on the worker right after the agent updated its totals, so reading them here is safe.
    ev.usage_updated = [this](const usage& u) {
        const auto& total = app_.core->total;
        push(ev_usage{u.context(), total.input + total.cache_read + total.cache_write, total.output});
    };
    ev.compacted = [this] { push(ev_notice{"上下文已自动压缩", app_.core->context_tokens}); };
    ev.notice = [this](std::string_view message) { push(ev_notice{std::string(message), std::nullopt}); };
    ev.take_steering = [this] {
        std::lock_guard lock(mutex_);
        auto taken = std::move(steering_);
        steering_.clear();
        return taken;
    };
    return ev;
}

void interactive_ui::start_job(std::function<turn_result(agent_events&, cancel_token&)> job, bool compact) {
    if (worker_.joinable()) worker_.join();
    cancel_.reset();
    busy_ = true;
    thinking_ = true;
    busy_since_ = steady::now();
    worker_ = std::thread([this, job = std::move(job), compact] {
        auto ev = make_events();
        turn_result r;
        try {
            r = job(ev, cancel_);
        } catch (const std::exception& e) {
            r = {stop_reason::error, e.what()};
        }
        push(ev_done{r, compact});
    });
}

void interactive_ui::start_turn(std::string text, std::vector<std::string> allowed_tools) {
    section("user");
    // Use cached theme color for user messages
    screen_.commit(ui::layout({{text, ui::tone::normal}}, size_.columns - 1,
                              {app_.current_theme.user_message_fg + "› \033[0m", 2}, {"  ", 2}));
    start_job([this, text = std::move(text), allowed_tools = std::move(allowed_tools)](agent_events& ev, cancel_token& c) {
        return app_.core->run(text, ev, c, {allowed_tools});
    }, false);
}

void interactive_ui::drain() {
    std::deque<ui_event> events;
    {
        std::lock_guard lock(mutex_);
        events.swap(queue_);
    }
    for (auto& e : events) std::visit([this](auto& v) { apply(v); }, e);
}

int interactive_ui::run(std::string first_prompt) {
    // Show welcome screen properly through screen_ (prevents duplication on resize)
    if (ui::should_show_welcome() && first_prompt.empty() && !welcome_shown_) {
        const auto welcome = ui::welcome_screen(size_.columns);
        screen_.commit(welcome);
        welcome_shown_ = true;
        ui::mark_welcome_shown();
    }

    if (!first_prompt.empty()) start_turn(std::move(first_prompt));
    auto last_draw = steady::time_point{};
    bool dirty = true;
    while (!quit_) {
        auto keys = term_.read(busy_ ? 16 : 33);  // 60fps when busy, 30fps when idle
        if (!keys) break;
        const auto now_size = term_.dimensions();
        if (now_size.columns != size_.columns) {
            // Rows already drawn were re-wrapped by the terminal; start the live area afresh.
            screen_.reset_view();
            dirty = true;
        }
        size_ = now_size;
        for (const auto& k : *keys) {
            handle(k);
            dirty = true;
            if (quit_) break;
        }
        bool queued = false;
        {
            std::lock_guard lock(mutex_);
            queued = !queue_.empty();
        }
        if (queued) {
            drain();
            dirty = true;
        }
        const auto now = steady::now();
        if (busy_ && now - last_draw > 100ms) dirty = true;
        if (!hint_.empty() && now - hint_at_ > 2s) {
            hint_.clear();
            ctrl_c_at_ = {};
            dirty = true;
        }
        if (dirty) {
            int row = -1, column = 0;
            // screen skips the write itself when nothing changed; checking rows here
            // would also hold back committed lines that are waiting to be flushed.
            const auto rows = live(row, column);
            screen_.draw(rows, row, column);
            last_draw = now;
            dirty = false;
        }
    }
    screen_.finish();
    return 0;
}

void interactive_ui::show_customize_menu() {
    std::vector<std::string> items = {
        "User message", "Assistant", "Tool: Read", "Tool: Write",
        "Tool: Edit", "Tool: Bash", "Accent", "Borders", "Error", "Success"
    };

    std::vector<std::string> details;
    auto& t = app_.current_theme;

    auto rgb_str = [](const rgb_color& c) {
        return c.ansi_fg() + "■■■\033[0m (" +
               std::to_string(c.r) + "," + std::to_string(c.g) + "," + std::to_string(c.b) + ")";
    };

    details.push_back(rgb_str(t.user_message));
    details.push_back(rgb_str(t.assistant_message));
    details.push_back(rgb_str(t.tool_read));
    details.push_back(rgb_str(t.tool_write));
    details.push_back(rgb_str(t.tool_edit));
    details.push_back(rgb_str(t.tool_bash));
    details.push_back(rgb_str(t.accent));
    details.push_back(rgb_str(t.border));
    details.push_back(rgb_str(t.error));
    details.push_back(rgb_str(t.success));

    selection_ = {
        items, details, 0,
        "Customize - Select element to edit:",
        [this](std::size_t) {
            section("assistant");
            screen_.commit(paint(ui::tone::accent, "RGB color editor coming soon!"));
            screen_.commit(paint(ui::tone::dim, "Will allow adjusting R/G/B values 0-255"));
        },
        []() {}
    };
}

} // namespace ode
