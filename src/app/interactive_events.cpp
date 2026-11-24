#include "app/interactive.hpp"

namespace lay {

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

void interactive_ui::apply(ev_tool_start& e) {
    thinking_ = false;
    flush_stream(true);
    running_.push_back({e.name, e.label, steady::now()});
    progress_tail_.clear();
}

void interactive_ui::apply(ev_progress& e) {
    progress_tail_ += e.text;
    if (progress_tail_.size() > 4096) progress_tail_.erase(0, progress_tail_.size() - 4096);
}

void interactive_ui::apply(ev_tool_done& e) {
    for (auto it = running_.begin(); it != running_.end(); ++it) {
        if (it->name == e.name && it->label == e.label) {
            running_.erase(it);
            break;
        }
    }
    if (running_.empty()) progress_tail_.clear();
    section("tool");
    const int columns = size_.columns - 1;
    const auto head = std::string(ui::verb(e.name));
    const auto summary = e.summary.empty() ? std::string{} : "  " + e.summary;
    const int fixed = 4 + text::width(head) + text::width(summary);
    const auto label = text::keep_head(e.label, std::max(8, columns - fixed));
    const auto dot = paint(e.error ? tone::error : tone::dim, "●");
    screen_.commit(" " + dot + " " + paint(tone::dim, head + " " + label) + paint(e.error ? tone::error : tone::dim, summary));
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
    if (e.error && e.name != "Bash" && !e.full.empty()) {
        note(text::keep_head(ui::one_line(e.full), columns - 4), tone::error);
    }
    last_tool_full_ = e.full;
}

void interactive_ui::apply(ev_ask& e) {
    flush_stream(true);
    ask_ = std::move(e);
}

void interactive_ui::answer_ask(answer a) {
    if (!ask_) return;
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

} // namespace lay
