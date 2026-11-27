#include "app/interactive.hpp"
#include "platform/file_lock.hpp"
#include "ui/welcome.hpp"

#include <fstream>

namespace ode {

using namespace std::chrono_literals;

namespace {

std::string_view bash_command(const json& input) {
    const auto command = input.find("command");
    return command != input.end() && command->is_string() ? command->get_ref<const std::string&>() : std::string_view{};
}

} // namespace

interactive_ui::interactive_ui(app& a, ui::terminal& term) : app_(a), term_(term), screen_(term) {
    size_ = term_.dimensions();
    branch_ = git_branch(app_.root);

    const auto history_path = ode_home() / "command_history.txt";
    if (fs::exists(history_path)) {
        app_.history.load(history_path);
    }
}

interactive_ui::~interactive_ui() {
    const auto history_path = ode_home() / "command_history.txt";
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

void interactive_ui::set_permission_mode(permission_mode mode) {
    app_.permissions.set_mode(mode);
    const auto directory = ode_home();
    const auto path = directory / "permission_mode.txt";
    std::error_code ec;
    fs::create_directories(directory, ec);
    if (!ec) {
        const file_lock lock(directory / "permission_mode.lock");
        if (lock) {
            const auto temporary = directory / "permission_mode.tmp";
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            out << mode_name(mode);
            out.close();
            if (out) {
                fs::rename(temporary, path, ec);
                if (!ec) return;
            }
            fs::remove(temporary, ec);
        }
    }
    note("权限档已切换，但无法保存到下次启动", ui::tone::error);
}

void interactive_ui::push(ui_event e) {
    std::lock_guard lock(mutex_);
    if (auto* update = std::get_if<ev_tool_preparing>(&e); update && update->started == steady::time_point{} && !queue_.empty()) {
        if (auto* previous = std::get_if<ev_tool_preparing>(&queue_.back());
            previous && previous->started == steady::time_point{} && previous->name == update->name) {
            previous->argument_bytes = update->argument_bytes;
            return;
        }
    }
    queue_.push_back(std::move(e));
}

agent_events interactive_ui::make_events() {
    agent_events ev;
    ev.request_started = [this] { push(ev_request{}); };
    ev.request_finished = [this](const request_timing& timing) { push(ev_request_done{timing}); };
    ev.text = [this](std::string_view t) { push(ev_text{std::string(t)}); };
    ev.thinking = [this](std::string_view text, std::string_view) {
        if (!text.empty()) push(ev_thinking{std::string(text)});
    };
    ev.tool_preparing = [this](std::string_view name) {
        if (!name.empty()) push(ev_tool_preparing{std::string(name), steady::now(), 0});
    };
    ev.tool_argument_progress = [this](std::string_view name, std::size_t bytes) {
        push(ev_tool_preparing{std::string(name), {}, bytes});
    };
    ev.tool_started = [this](const tool& t, const json& in) {
        const auto command = t.name == "Bash" ? bash_command(in) : std::string_view{};
        const auto label = t.name == "Bash" ? ui::one_line(command) : tool_label(t, in, app_.root);
        push(ev_tool_start{t.name, label, std::string(command), steady::now()});
    };
    ev.tool_progress = [this](std::string_view s) { push(ev_progress{std::string(s)}); };
    ev.tool_finished = [this](const tool& t, const json& in, const tool_output& o) {
        const auto label = t.name == "Bash" ? ui::one_line(bash_command(in)) : tool_label(t, in, app_.root);
        push(ev_tool_done{t.name, label, o.summary, o.detail, o.text, o.is_error, o.elapsed});
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

void interactive_ui::start_job(std::function<turn_result(agent_events&, cancel_token&)> job, bool compact, bool needs_provider) {
    if (needs_provider && app_.credential_error) {
        note(*app_.credential_error, ui::tone::error);
        return;
    }
    if (worker_.joinable()) worker_.join();
    cancel_.reset();
    busy_ = true;
    thinking_ = true;
    responding_ = false;
    request_summary_.clear();
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

void interactive_ui::show_user_message(std::string_view message) {
    section("user");
    const int columns = std::max(1, size_.columns - 1);
    const auto wrapped = text::wrap(message, std::max(1, columns - 8));
    int content_width = 0;
    for (const auto& line : wrapped) content_width = std::max(content_width, text::width(line));
    const int block_width = std::min(columns - 1, content_width + 7);
    const bool color = ui::color_enabled();
    const auto background = color ? app_.current_theme.user_message_bg : std::string{};
    const auto foreground = color ? "\033[39m" : "";
    const auto reset = color ? "\033[0m" : "";
    for (std::size_t i = 0; i < wrapped.size(); ++i) {
        const auto prefix = i == 0 ? "› " : "  ";
        const int used = 1 + text::width(prefix) + text::width(wrapped[i]);
        const int padding = color ? std::max(0, block_width - used) : 0;
        screen_.commit(" " + background + foreground + prefix + wrapped[i] +
                       std::string(static_cast<std::size_t>(padding), ' ') + reset);
    }
}

void interactive_ui::start_turn(std::string text, std::vector<std::string> allowed_tools) {
    show_user_message(text);
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

void interactive_ui::show_session_history() {
    for (const auto& message : app_.core->messages) {
        if (message.from == message::role::user) {
            const auto text = message.plain_text();
            if (!text.empty()) show_user_message(text);
            continue;
        }
        for (const auto& block : message.blocks) {
            if (block.type == block::kind::text && !block.text.empty()) {
                section("assistant");
                print_markdown(block.text);
            } else if (block.type == block::kind::tool_use && block.name == "Bash") {
                show_shell_command(bash_command(block.input));
            }
        }
    }
}

int interactive_ui::run(std::string first_prompt) {
    const bool has_shell_output = screen_.start();
    if (ui::should_show_welcome() && first_prompt.empty() && app_.core->messages.empty() && !welcome_shown_) {
        if (has_shell_output || size_.rows < 25 || size_.columns < 24) {
            note("输入 /help 查看命令和快捷键");
        } else {
            screen_.commit(ui::welcome_screen(size_.columns));
        }
        welcome_shown_ = true;
        ui::mark_welcome_shown();
    }

    show_session_history();
    if (!first_prompt.empty()) start_turn(std::move(first_prompt));
    auto last_draw = steady::time_point{};
    bool dirty = true;
    while (!quit_) {
        auto keys = term_.read(busy_ ? 16 : 33);  // 60fps when busy, 30fps when idle
        if (!keys) break;
        const auto now_size = term_.dimensions();
        if (now_size != size_) dirty = true;
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
            const auto footer = footer_rows(row, column);
            screen_.draw(size_, activity_rows(), footer, row, column);
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
