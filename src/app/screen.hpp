#pragma once

#include "ui/term.hpp"

#include <string>
#include <vector>

namespace lay {

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
        std::string out = "\x1b[?2026h\x1b[?25l";
        erase(out);
        for (const auto& line : pending_) out += line + "\x1b[0m\x1b[K\r\n";
        pending_.clear();
        for (std::size_t i = 0; i < live.size(); ++i) {
            out += live[i] + "\x1b[0m\x1b[K";
            if (i + 1 < live.size()) out += "\r\n";
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
        out += "\x1b[?2026l";
        term_.write(out);
    }

    // Removes the live area and flushes pending output, leaving the cursor on a fresh line.
    void finish() {
        std::string out;
        erase(out);
        for (const auto& line : pending_) out += line + "\x1b[0m\r\n";
        pending_.clear();
        out += "\x1b[?25h";
        drawn_ = false;
        term_.write(out);
    }

    void clear_screen() {
        term_.write("\x1b[2J\x1b[3J\x1b[H");
        drawn_ = false;
        up_ = 0;
    }

    // After a width change the old live rows cannot be located; clear the viewport only.
    void reset_view() {
        term_.write("\x1b[2J\x1b[H");
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

    ui::terminal& term_;
    std::vector<std::string> pending_;
    int up_ = 0;
    bool drawn_ = false;
};

} // namespace lay
