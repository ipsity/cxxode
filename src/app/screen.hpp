#pragma once

#include "ui/term.hpp"

#include <deque>
#include <string>
#include <vector>

namespace ode {

// Inline renderer. Finished output scrolls into the terminal's own history;
// only the live area at the bottom (activity, input box, status) is redrawn.
class screen {
public:
    explicit screen(ui::terminal& term) : term_(term) {}

    void commit(std::string line) { pending_.push_back(std::move(line)); }
    void commit(const std::vector<std::string>& lines) {
        pending_.insert(pending_.end(), lines.begin(), lines.end());
    }
    bool has_pending() const { return !pending_.empty(); }

    // Replaces the live area. cursor_row < 0 hides the cursor.
    void draw(const std::vector<std::string>& live, int cursor_row, int cursor_column) {
        if (drawn_ && pending_.empty() && live == last_live_ && cursor_row == last_cursor_row_ &&
            cursor_column == last_cursor_column_)
            return;

        std::string out = "\x1b[?2026h\x1b[?25l";
        if (drawn_ && pending_.empty() && live.size() == last_live_.size()) {
            // Same height and nothing scrolls in: overwrite changed rows in place. Erasing
            // first would blank the rows we skip.
            out += "\r";
            if (up_ > 0) out += "\x1b[" + std::to_string(up_) + "A";
            for (std::size_t i = 0; i < live.size(); ++i) {
                if (live[i] != last_live_[i]) out += live[i] + "\x1b[0m\x1b[K";
                if (i + 1 < live.size()) out += "\r\n";
            }
        } else {
            erase(out);
            flush_pending(out);
            for (std::size_t i = 0; i < live.size(); ++i) {
                out += live[i] + "\x1b[0m\x1b[K";
                if (i + 1 < live.size()) out += "\r\n";
            }
        }

        const int last = static_cast<int>(live.size()) - 1;
        if (cursor_row >= 0 && cursor_row <= last) {
            if (last - cursor_row) out += "\x1b[" + std::to_string(last - cursor_row) + "A";
            out += "\r";
            if (cursor_column > 0) out += "\x1b[" + std::to_string(cursor_column) + "C";
            out += "\x1b[?25h";
            up_ = cursor_row;
        } else {
            up_ = last < 0 ? 0 : last;
        }

        drawn_ = !live.empty();
        last_live_ = live;
        last_cursor_row_ = cursor_row;
        last_cursor_column_ = cursor_column;
        out += "\x1b[?2026l";
        term_.write(out);
    }

    // Removes the live area and flushes pending output, leaving the cursor on a fresh line.
    void finish() {
        std::string out;
        erase(out);
        flush_pending(out);
        out += "\x1b[?25h";
        term_.write(out);
    }

    void clear_screen() {
        term_.write("\x1b[2J\x1b[3J\x1b[H");
        history_.clear();
        drawn_ = false;
        up_ = 0;
    }

    // After a width change the terminal has re-wrapped everything, so the old live rows
    // cannot be located. Wipe the scrollback (otherwise stale live areas show up as
    // duplicates) and replay the committed history so the conversation is not lost.
    void reset_view() {
        std::string out = "\x1b[2J\x1b[3J\x1b[H\x1b[0m";
        for (const auto& line : history_) out += line + "\x1b[0m\x1b[K\r\n";
        term_.write(out);
        drawn_ = false;
        up_ = 0;
    }

private:
    void erase(std::string& out) {
        if (!drawn_) return;
        out += "\r";
        if (up_ > 0) out += "\x1b[" + std::to_string(up_) + "A";
        out += "\x1b[J";
        drawn_ = false;
    }

    void flush_pending(std::string& out) {
        for (auto& line : pending_) {
            out += line + "\x1b[0m\x1b[K\r\n";
            history_.push_back(std::move(line));
        }
        pending_.clear();
        while (history_.size() > max_history) history_.pop_front();
    }

    // Bounds the replay on resize; older lines are rarely scrolled back to.
    static constexpr std::size_t max_history = 5000;

    ui::terminal& term_;
    std::vector<std::string> pending_;
    std::deque<std::string> history_;
    std::vector<std::string> last_live_;
    int last_cursor_row_ = -1;
    int last_cursor_column_ = 0;
    int up_ = 0;
    bool drawn_ = false;
};

} // namespace ode
