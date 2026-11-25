#pragma once

#include "ui/render.hpp"
#include "ui/term.hpp"

#include <algorithm>
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace ode {

class screen {
public:
    explicit screen(ui::terminal& term) : term_(term) {}
    ~screen() { if (active_) term_.write("\x1b[?1049l"); }

    bool start() {
        for (auto& line : term_.visible_lines()) commit(std::move(line));
        first_session_row_ = first_row_ + history_.size();
        term_.write("\x1b[?1049h");
        active_ = true;
        return !history_.empty();
    }

    void commit(std::string line) {
        history_.push_back(std::move(line));
        if (history_.size() > max_history) {
            history_.pop_front();
            ++first_row_;
        }
    }

    void commit(std::vector<std::string> lines) {
        for (auto& line : lines) commit(std::move(line));
    }

    void scroll(int delta) {
        if (delta < 0) {
            const auto top = std::max(first_row_, anchor_.value_or(tail_top_));
            anchor_ = top - std::min(top - first_row_, static_cast<std::size_t>(-delta));
            if (*anchor_ >= tail_top_) anchor_.reset();
        } else if (anchor_) {
            *anchor_ += static_cast<std::size_t>(delta);
            if (*anchor_ >= tail_top_) anchor_.reset();
        }
    }

    int page_size() const { return std::max(1, visible_rows_ - 1); }

    void draw(ui::size size, const std::vector<std::string>& activity,
              const std::vector<std::string>& footer, int cursor_row, int cursor_column) {
        const auto height = static_cast<std::size_t>(size.rows);
        const auto footer_start = footer.size() > height ? footer.size() - height : 0;
        const auto capacity = height - (footer.size() - footer_start);
        visible_rows_ = static_cast<int>(capacity);
        const auto total = history_.size() + activity.size();
        const auto tail = total - std::min(total, capacity);
        tail_top_ = first_row_ + tail;
        const auto top = std::min(total, std::max(first_row_, anchor_.value_or(tail_top_)) - first_row_);

        std::vector<std::string> frame;
        frame.reserve(height);
        for (auto i = top; i < std::min(total, top + capacity); ++i) {
            frame.push_back(i < history_.size() ? history_[i] : activity[i - history_.size()]);
        }
        frame.resize(capacity);
        frame.insert(frame.end(), footer.begin() + footer_start, footer.end());
        if (cursor_row >= static_cast<int>(footer_start))
            cursor_row += static_cast<int>(capacity) - static_cast<int>(footer_start);
        else cursor_row = -1;

        const auto columns = std::max(0, size.columns - 1);
        cursor_column = std::min(cursor_column, columns);
        for (auto& line : frame) line = ui::clip_styled_line(line, columns);
        if (size == last_size_ && frame == last_frame_ && cursor_row == last_cursor_row_ &&
            cursor_column == last_cursor_column_) return;

        std::string out = "\x1b[?2026h\x1b[?25l";
        for (std::size_t i = 0; i < frame.size(); ++i) {
            if (size != last_size_ || i >= last_frame_.size() || frame[i] != last_frame_[i]) {
                out += "\x1b[" + std::to_string(i + 1) + ";1H" + frame[i] + "\x1b[0m\x1b[K";
            }
        }
        if (cursor_row >= 0) {
            out += "\x1b[" + std::to_string(cursor_row + 1) + ";" + std::to_string(cursor_column + 1) + "H\x1b[?25h";
        }
        out += "\x1b[?2026l";
        term_.write(out);
        last_frame_ = std::move(frame);
        last_size_ = size;
        last_cursor_row_ = cursor_row;
        last_cursor_column_ = cursor_column;
    }

    void finish() {
        term_.write("\x1b[?1049l\x1b[0m\x1b[?25h");
        active_ = false;
        // Keep the new transcript available in the shell after the alternate screen closes.
        const auto columns = std::max(0, term_.dimensions().columns - 1);
        for (auto i = std::max(first_session_row_, first_row_) - first_row_; i < history_.size(); ++i) {
            term_.write(ui::clip_styled_line(history_[i], columns) + "\x1b[0m\r\n");
        }
    }

    void clear_screen() {
        first_row_ += history_.size();
        history_.clear();
        anchor_.reset();
        tail_top_ = first_row_;
    }

private:
    static constexpr std::size_t max_history = 5000;
    ui::terminal& term_;
    std::deque<std::string> history_;
    // An absolute row keeps the viewport anchored when new output arrives or old rows expire.
    std::size_t first_row_ = 0;
    std::size_t first_session_row_ = 0;
    std::size_t tail_top_ = 0;
    std::optional<std::size_t> anchor_;
    int visible_rows_ = 0;
    std::vector<std::string> last_frame_;
    ui::size last_size_{};
    int last_cursor_row_ = -1;
    int last_cursor_column_ = 0;
    bool active_ = false;
};

} // namespace ode
