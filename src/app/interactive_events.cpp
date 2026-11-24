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
    thinking_ = false;
    // Skip leading blank lines so the reply starts right under the tool list.
    if (stream_.empty() && last_section_ != "assistant") {
        const auto first = e.text.find_first_not_of("\r\n");
        if (first == std::string::npos) return;
        e.text.erase(0, first);
    }
    stream_ += e.text;
    flush_stream(false);
}

void interactive_ui::apply(ev_thinking& e) {
    flush_stream(true);
    // Track thinking block - collapsed by default
    thinking_blocks_.push_back({e.signature, false});
    section("assistant");

    if (thinking_blocks_.back().second) {
        // Expanded: show full text
        for (auto& row : ui::layout({{e.text, ui::tone::dim}}, size_.columns - 1, {"  💭 ", 4}, {"    ", 4})) {
            screen_.commit(row);
        }
    } else {
        // Collapsed: show summary
        const auto summary = text::keep_head(ui::one_line(e.text), size_.columns - 20);
        screen_.commit("  " + paint(ui::tone::dim, "💭 " + summary + " ⋯ Ctrl+T 展开"));
    }
}

void interactive_ui::apply(ev_tool_start& e) {
    thinking_ = false;
    flush_stream(true);
    running_.push_back({e.name, e.label, steady::now(), -1, "", 0, 0, 0, 0});
    progress_tail_.clear();
}

void interactive_ui::apply(ev_progress& e) {
    // 处理传统文本进度（兼容）
    if (!e.text.empty()) {
        progress_tail_ += e.text;
        if (progress_tail_.size() > 4096) progress_tail_.erase(0, progress_tail_.size() - 4096);
    }

    // 更新详细进度信息到对应的 running_tool
    if (e.percent >= 0 && !e.tool_name.empty()) {
        for (auto& tool : running_) {
            if (tool.name == e.tool_name) {
                tool.percent = e.percent;
                tool.current_step = e.current_step;
                tool.elapsed_ms = e.elapsed_ms;
                tool.estimated_ms = e.estimated_ms;
                tool.bytes_io = e.bytes_io;
                tool.memory_kb = e.memory_kb;
                break;
            }
        }
    }
}

void interactive_ui::apply(ev_tool_done& e) {
    for (auto it = running_.begin(); it != running_.end(); ++it) {
        if (it->name == e.name && it->label == e.label) {
            running_.erase(it);
            break;
        }
    }
    if (running_.empty()) progress_tail_.clear();

    // Day 8: Record tool usage for suggestion learning
    app_.suggestion_engine.record_tool_use(e.name);

    section("tool");
    const int columns = size_.columns - 1;
    const auto head = std::string(ui::verb(e.name));
    const auto summary = e.summary.empty() ? std::string{} : "  " + e.summary;
    const int fixed = 4 + text::width(head) + text::width(summary);
    const auto label = text::keep_head(e.label, std::max(8, columns - fixed));
    const auto dot = paint(e.error ? tone::error : tone::dim, "●");

    // Show tool completion with label (command/file)
    screen_.commit(" " + dot + " " + paint(tone::dim, head + " ") + paint(tone::accent, label) + paint(e.error ? tone::error : tone::dim, summary));

    // Show full command for Bash
    if (e.name == "Bash" && !e.label.empty() && e.label.size() < 200) {
        screen_.commit("   " + paint(tone::dim, "│ $ ") + paint(tone::accent, e.label));
    }

    // Show detail (diff/output)
    if (!e.detail.empty()) {
        std::size_t start = 0;
        while (start < e.detail.size()) {
            auto end = e.detail.find('\n', start);
            if (end == std::string::npos) end = e.detail.size();
            const auto line = std::string_view(e.detail).substr(start, end - start);
            const auto t = line.starts_with("-") ? tone::error : line.starts_with("+") ? tone::accent : tone::dim;
            screen_.commit("   " + paint(tone::dim, "│ ") + paint(t, text::keep_head(text::expand_tabs(line), columns - 6)));
            start = end + 1;
        }
    }

    // Show full output for errors - Day 9: Enhanced with friendly error handling
    if (e.error && !e.full.empty()) {
        // Analyze error with error_handler
        auto error_info = app_.error_handler_.analyze(e.full, e.name, e.label);

        // Show friendly error message
        if (e.name != "Bash") {
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

        // For Bash, still show original output (more context needed)
        if (e.name == "Bash") {
            note(text::keep_head(ui::one_line(e.full), columns - 4), tone::error);
        }
    }
    last_tool_full_ = e.full;
}

void interactive_ui::apply(ev_ask& e) {
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

void interactive_ui::apply(ev_done& e) {
    flush_stream(true);
    running_.clear();
    progress_tail_.clear();
    busy_ = false;
    thinking_ = false;
    if (worker_.joinable()) worker_.join();
    const auto& r = e.result;
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
