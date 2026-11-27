#include "app/interactive.hpp"

namespace ode {

using ui::paint;
using ui::tone;

void interactive_ui::blank() { screen_.commit(std::string{}); }

void interactive_ui::note(std::string_view text, tone t) {
    for (auto& row : ui::layout({{std::string(text), t}}, size_.columns - 1, {"  ", 2}, {"  ", 2})) screen_.commit(row);
}

void interactive_ui::section(std::string_view kind) {
    // One blank line between a user prompt, the assistant's prose and a run of tool calls.
    if (!last_section_.empty() && last_section_ != kind) blank();
    else if (kind == "user" && !last_section_.empty()) blank();
    last_section_ = kind;
}

void interactive_ui::print_markdown(std::string_view text) {
    ui::markdown_state state;
    std::size_t start = 0;
    while (start <= text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string_view::npos) end = text.size();
        screen_.commit(ui::markdown_line(text.substr(start, end - start), state, size_.columns - 1));
        if (end == text.size()) break;
        start = end + 1;
    }
}

// Commits complete lines of streamed text; the unfinished line stays live.
void interactive_ui::flush_stream(bool all) {
    std::size_t start = 0;
    while (true) {
        const auto nl = stream_.find('\n', start);
        if (nl == std::string::npos) break;
        const auto line = std::string_view(stream_).substr(start, nl - start);
        section("assistant");
        screen_.commit(ui::markdown_line(line, md_, size_.columns - 1));
        start = nl + 1;
    }
    stream_.erase(0, start);
    if (all && !stream_.empty()) {
        section("assistant");
        screen_.commit(ui::markdown_line(stream_, md_, size_.columns - 1));
        stream_.clear();
    }
    if (all) md_ = {};
}

void interactive_ui::apply(ev_text& e) {
    if (e.text.empty()) return;
    flush_thinking(true);
    thinking_ = false;
    if (!responding_) response_started_ = steady::now();
    responding_ = true;
    // Skip leading blank lines so the reply starts right under the tool list.
    if (stream_.empty() && last_section_ != "assistant") {
        const auto first = e.text.find_first_not_of("\r\n");
        if (first == std::string::npos) return;
        e.text.erase(0, first);
    }
    section("assistant");
    stream_ += e.text;
    flush_stream(false);
}

std::vector<std::string> interactive_ui::thinking_rows(std::string_view line, int columns) const {
    const ui::prefix prefix{paint(tone::dim, "  ┊ "), text::width("  ┊ ")};
    return ui::layout({{std::string(line), tone::thinking}}, columns, prefix, prefix);
}

void interactive_ui::flush_thinking(bool all) {
    if (!all) return;
    std::size_t start = 0;
    while (start < think_stream_.size()) {
        const auto end = think_stream_.find('\n', start);
        const auto stop = end == std::string::npos ? think_stream_.size() : end;
        screen_.commit(thinking_rows(std::string_view(think_stream_).substr(start, stop - start), size_.columns - 1));
        start = end == std::string::npos ? stop : end + 1;
    }
    if (!think_stream_.empty()) screen_.commit(std::string{});
    think_stream_.clear();
    if (hidden_thinking_chars_ != 0) {
        note("✻ 思考 " + std::to_string(hidden_thinking_chars_) + " 字");
        hidden_thinking_chars_ = 0;
    }
    think_started_ = false;
}

void interactive_ui::apply(ev_thinking& e) {
    if (!think_started_) {
        flush_stream(true);
        section("assistant");
        if (show_thinking_) note("✻ 思考");
        think_started_ = true;
    }
    if (show_thinking_) {
        think_stream_ += e.text;
        flush_thinking(false);
    } else {
        hidden_thinking_chars_ += std::count_if(e.text.begin(), e.text.end(), [](unsigned char byte) {
            return (byte & 0xc0) != 0x80;
        });
    }
}

void interactive_ui::apply(ev_request&) {
    flush_thinking(true);
    flush_stream(true);
    preparing_tool_.reset();
    thinking_ = true;
    thinking_status_started_ = steady::now();
    request_summary_.clear();
    responding_ = false;
}

void interactive_ui::apply(ev_request_done& e) {
    if (e.timing.compact) {
        request_summary_.clear();
        thinking_ = false;
        responding_ = false;
        return;
    }
    request_summary_ = "✻ 思考完成 " + format_elapsed(e.timing.elapsed);
    if (e.timing.first_output) request_summary_ += " · 首输出 " + format_elapsed(*e.timing.first_output);
    if (e.timing.provider) {
        request_summary_ += " · 编码 " + format_elapsed(e.timing.provider->encode);
        request_summary_ += " · 网络/流 " + format_elapsed(e.timing.provider->http);
    }
    if (hidden_thinking_chars_ != 0) {
        request_summary_ += " · 思考 " + std::to_string(hidden_thinking_chars_) + " 字";
        hidden_thinking_chars_ = 0;
    }
    thinking_ = false;
    responding_ = false;
}

void interactive_ui::apply(ev_tool_preparing& e) {
    flush_thinking(true);
    if (e.started != steady::time_point{}) {
        preparing_tool_ = std::move(e);
    } else if (preparing_tool_ && preparing_tool_->name == e.name) {
        preparing_tool_->argument_bytes = e.argument_bytes;
    }
    thinking_ = false;
    responding_ = false;
}

void interactive_ui::show_shell_command(std::string_view command) {
    if (command.empty()) return;
    section("tool");
    const ui::prefix first{"   " + paint(tone::dim, "│ $ "), 7};
    const ui::prefix next{"   " + paint(tone::dim, "│   "), 7};
    screen_.commit(ui::layout({{std::string(command), tone::accent}}, size_.columns - 1, first, next));
}

std::vector<std::string> interactive_ui::tool_detail_rows(std::string_view line, tone style) const {
    if (line.ends_with('\r')) line.remove_suffix(1);
    if (const auto last = line.rfind('\r'); last != std::string_view::npos) line.remove_prefix(last + 1);
    const auto prefix = "   " + paint(tone::dim, "│ ");
    auto rows = text::wrap_line(line, size_.columns - 6);
    for (auto& row : rows) row = prefix + paint(style, row);
    return rows;
}

void interactive_ui::flush_command_output(bool all) {
    std::size_t start = 0;
    for (auto end = progress_tail_.find('\n'); end != std::string::npos; end = progress_tail_.find('\n', start)) {
        screen_.commit(tool_detail_rows(std::string_view(progress_tail_).substr(start, end - start), tone::dim));
        start = end + 1;
    }
    progress_tail_.erase(0, start);
    if (all && !progress_tail_.empty()) {
        screen_.commit(tool_detail_rows(progress_tail_, tone::dim));
        progress_tail_.clear();
    }
}

void interactive_ui::apply(ev_tool_start& e) {
    flush_thinking(true);
    preparing_tool_.reset();
    thinking_ = false;
    flush_stream(true);
    running_.push_back({e.name, e.label, e.started});
    if (e.name == "Bash") {
        command_output_bytes_ = 0;
        command_output_truncated_ = false;
        show_shell_command(e.command);
    }
}

void interactive_ui::apply(ev_progress& e) {
    constexpr std::size_t output_limit = 30000;
    if (command_output_truncated_) return;
    auto count = std::min(e.text.size(), output_limit - command_output_bytes_);
    if (count < e.text.size()) {
        while (count > 0 && (static_cast<unsigned char>(e.text[count]) & 0xc0) == 0x80) --count;
        command_output_truncated_ = true;
    }
    progress_tail_.append(e.text, 0, count);
    command_output_bytes_ += count;
    flush_command_output(command_output_truncated_);
    if (command_output_truncated_)
        screen_.commit(tool_detail_rows("… 输出已截断，Ctrl+O 查看工具保留的输出", tone::dim));
}

void interactive_ui::apply(ev_tool_done& e) {
    for (auto it = running_.begin(); it != running_.end(); ++it) {
        if (it->name == e.name && it->label == e.label) {
            running_.erase(it);
            break;
        }
    }
    const bool bash = e.name == "Bash";
    if (bash) flush_command_output(true);

    app_.suggestion_engine.record_tool_use(e.name);

    section("tool");
    const int columns = size_.columns - 1;
    const auto head = std::string(ui::verb(e.name));
    const auto duration = e.elapsed ? "  " + format_elapsed(*e.elapsed) : std::string{};
    const auto summary = e.summary.empty() ? std::string{} : "  " + e.summary;
    const int fixed = 4 + text::width(head) + text::width(duration) + text::width(summary);
    const auto label = bash ? std::string{} : text::keep_head(e.label, std::max(0, columns - fixed));
    const auto status = e.error ? paint(tone::failure, "✗ 失败") : paint(tone::success, "✓ 成功");

    screen_.commit(" " + status + "  " + paint(tone::bold, head) + paint(tone::dim, duration) +
                   (label.empty() ? "" : " " + paint(tone::accent, label)) +
                   paint(e.error ? tone::failure : tone::dim, summary));

    if (!e.detail.empty()) {
        std::size_t start = 0;
        while (start < e.detail.size()) {
            auto end = e.detail.find('\n', start);
            if (end == std::string::npos) end = e.detail.size();
            const auto line = std::string_view(e.detail).substr(start, end - start);
            auto style = tone::dim;
            if (line.starts_with("- ")) style = tone::diff_removed;
            else if (line.starts_with("+ ")) style = tone::diff_added;
            screen_.commit(tool_detail_rows(line, style));
            start = end + 1;
        }
    }

    // Show full output for errors - Day 9: Enhanced with friendly error handling
    if (e.error && !e.full.empty()) {
        // Analyze error with error_handler
        auto error_info = app_.error_handler_.analyze(e.full, e.name, e.label);

        // Show friendly error message
        if (!bash) {
            screen_.commit("   " + paint(tone::error, "❌ " + error_info.friendly_message));
        }

        // Show recovery suggestions
        auto suggestions = app_.error_handler_.suggest_recovery(error_info);
        if (!suggestions.empty() && suggestions.size() <= 3) {
            screen_.commit("   " + paint(tone::dim, "💡 建议:"));
            for (size_t i = 0; i < std::min<size_t>(suggestions.size(), 3); ++i) {
                const auto& sugg = suggestions[i];
                std::string line = "      " + std::to_string(i + 1) + ". " + sugg.action;
                screen_.commit("   " + paint(tone::dim, line));
            }
        }

        if (bash && command_output_bytes_ == 0) {
            note(e.full, tone::error);
        }
    }
    last_tool_full_ = e.full;
}

void interactive_ui::apply(ev_ask& e) {
    if (cancel_.cancelled()) {
        e.reply->set_value(answer::no);
        return;
    }
    preparing_tool_.reset();
    flush_thinking(true);
    flush_stream(true);

    // Check if this exact request was recently processed (within 10 seconds)
    const auto now = steady::now();
    const auto key = e.name + "|" + e.label;

    // Clean up old entries (older than 10 seconds)
    recent_asks_.erase(
        std::remove_if(recent_asks_.begin(), recent_asks_.end(),
                      [&](const recent_ask& r) {
                          return std::chrono::duration_cast<std::chrono::seconds>(now - r.when).count() > 10;
                      }),
        recent_asks_.end()
    );

    // Check if this is a duplicate request
    for (const auto& recent : recent_asks_) {
        if (recent.key == key) {
            // Duplicate detected - use cached answer and don't show UI
            e.reply->set_value(recent.response);
            return;
        }
    }

    // Also prevent duplicate if one is already pending
    if (ask_ && ask_->name == e.name && ask_->label == e.label) {
        e.reply->set_value(answer::no);
        return;
    }

    ask_ = std::move(e);
}

void interactive_ui::answer_ask(answer a) {
    if (!ask_) return;

    // Record this decision to prevent duplicate asks
    const auto key = ask_->name + "|" + ask_->label;
    recent_asks_.push_back({key, steady::now(), a});

    if (a == answer::no) {
        section("tool");
        const auto line = std::string(ui::verb(ask_->name)) + " " + ask_->label;
        screen_.commit(" " + paint(tone::dim, "○ " + text::keep_head(line, size_.columns - 12) + "  已拒绝"));
    }
    ask_->reply->set_value(a);
    ask_.reset();
}

void interactive_ui::apply(ev_notice& e) {
    flush_thinking(true);
    flush_stream(true);
    note(e.message);
    if (e.context) usage_.context = *e.context;
}

void interactive_ui::apply(ev_done& e) {
    flush_thinking(true);
    flush_stream(true);
    flush_command_output(true);
    running_.clear();
    preparing_tool_.reset();
    busy_ = false;
    thinking_ = false;
    responding_ = false;
    if (worker_.joinable()) worker_.join();
    const auto& r = e.result;
    if (lua_key_) {
        const auto key = *lua_key_;
        lua_key_.reset();
        if (!lua_key_handled_ && r.stop != stop_reason::cancelled) handle(key, false);
    }
    if (r.stop == stop_reason::cancelled) {
        note("已中断", tone::dim);
    } else if (r.stop == stop_reason::error || !r.error.empty()) {
        note(r.error.empty() ? "出错了" : r.error, r.stop == stop_reason::error ? tone::error : tone::dim);
    } else if (e.compact) {
        note("上下文已压缩", tone::dim);
        usage_.context = app_.core->context_tokens;
    }
    // Text typed while the model was finishing becomes the next turn.
    std::vector<std::string> leftover;
    {
        std::lock_guard lock(mutex_);
        leftover = std::move(steering_);
        steering_.clear();
    }
    if (!leftover.empty() && r.stop != stop_reason::cancelled) {
        std::string joined;
        for (auto& s : leftover) joined += (joined.empty() ? "" : "\n") + s;
        start_turn(std::move(joined));
    } else if (!leftover.empty()) {
        std::string joined;
        for (auto& s : leftover) joined += (joined.empty() ? "" : "\n") + s;
        editor_.set(joined);
    }
}

} // namespace ode
