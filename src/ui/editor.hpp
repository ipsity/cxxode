#pragma once

#include "ui/text.hpp"

#include <string>
#include <vector>

namespace lay::ui {

// Multi-line draft with grapheme-aware cursor movement and input history.
class editor {
public:
    struct row {
        std::size_t begin, end;  // byte range in the draft
    };
    struct layout {
        std::vector<row> rows;
        int cursor_row = 0;
        int cursor_column = 0;
    };

    const std::string& text() const { return text_; }
    std::size_t cursor() const { return cursor_; }
    bool empty() const { return text_.empty(); }

    // Extracts the @ mention at the cursor: returns the text after @ up to the cursor.
    // Returns nullopt if cursor is not inside a @mention pattern.
    std::optional<std::string> at_mention() const {
        if (cursor_ == 0) return std::nullopt;
        // Find the last @ before the cursor
        auto pos = cursor_;
        while (pos > 0 && text_[pos - 1] != '@' && text_[pos - 1] != ' ' && text_[pos - 1] != '\n') --pos;
        if (pos == 0 || text_[pos - 1] != '@') return std::nullopt;
        return std::string(text_.substr(pos, cursor_ - pos));
    }

    void set(std::string value) {
        text_ = std::move(value);
        cursor_ = text_.size();
    }
    void clear() { set({}); }

    void insert(std::string_view s) {
        text_.insert(cursor_, s);
        cursor_ += s.size();
        browsing_ = false;
    }
    void backspace() {
        if (cursor_ == 0) return;
        const auto at = text::prev_boundary(text_, cursor_);
        text_.erase(at, cursor_ - at);
        cursor_ = at;
    }
    void erase_forward() {
        if (cursor_ >= text_.size()) return;
        text_.erase(cursor_, text::next_boundary(text_, cursor_) - cursor_);
    }
    void left() { if (cursor_ > 0) cursor_ = text::prev_boundary(text_, cursor_); }
    void right() { if (cursor_ < text_.size()) cursor_ = text::next_boundary(text_, cursor_); }
    void home() {
        const auto nl = cursor_ == 0 ? std::string::npos : text_.rfind('\n', cursor_ - 1);
        cursor_ = nl == std::string::npos ? 0 : nl + 1;
    }
    void end() {
        const auto nl = text_.find('\n', cursor_);
        cursor_ = nl == std::string::npos ? text_.size() : nl;
    }
    void word_left() {
        while (cursor_ > 0 && text_[cursor_ - 1] == ' ') left();
        while (cursor_ > 0 && text_[cursor_ - 1] != ' ' && text_[cursor_ - 1] != '\n') left();
    }
    void word_right() {
        while (cursor_ < text_.size() && text_[cursor_] != ' ' && text_[cursor_] != '\n') right();
        while (cursor_ < text_.size() && text_[cursor_] == ' ') right();
    }
    // Deletes from the start of the line to the cursor (Ctrl+U).
    void kill_line_before() {
        const auto keep = cursor_;
        home();
        text_.erase(cursor_, keep - cursor_);
    }

    // Moves within the wrapped draft. Returns false at the first or last row,
    // where the caller switches to history instead.
    bool vertical(int delta, int columns) {
        const auto l = arrange(columns);
        const int target = l.cursor_row + delta;
        if (target < 0 || target >= static_cast<int>(l.rows.size())) return false;
        const auto& r = l.rows[static_cast<std::size_t>(target)];
        int used = 0;
        cursor_ = r.end;
        for (const auto& c : text::cells(std::string_view(text_).substr(r.begin, r.end - r.begin))) {
            if (used + c.width > l.cursor_column) {
                cursor_ = r.begin + c.offset;
                break;
            }
            used += c.width;
        }
        return true;
    }

    void remember(const std::string& entry) {
        if (!entry.empty() && (history_.empty() || history_.back() != entry)) history_.push_back(entry);
        browsing_ = false;
    }
    void history_prev() {
        if (history_.empty()) return;
        if (!browsing_) {
            stash_ = text_;
            index_ = history_.size();
            browsing_ = true;
        }
        if (index_ > 0) set(history_[--index_]);
    }
    void history_next() {
        if (!browsing_) return;
        if (index_ + 1 < history_.size()) {
            set(history_[++index_]);
        } else {
            set(stash_);
            browsing_ = false;
        }
    }

    layout arrange(int columns) const {
        layout l;
        if (columns < 2) columns = 2;
        std::size_t line_start = 0;
        bool cursor_set = false;
        while (true) {
            const auto nl = text_.find('\n', line_start);
            const auto line_end = nl == std::string::npos ? text_.size() : nl;
            const auto line = std::string_view(text_).substr(line_start, line_end - line_start);
            std::size_t row_start = line_start;
            int used = 0;
            for (const auto& c : text::cells(line)) {
                const auto at = line_start + c.offset;
                if (used + c.width > columns) {
                    l.rows.push_back({row_start, at});
                    row_start = at;
                    used = 0;
                }
                if (!cursor_set && at == cursor_) {
                    l.cursor_row = static_cast<int>(l.rows.size());
                    l.cursor_column = used;
                    cursor_set = true;
                }
                used += c.width;
            }
            if (!cursor_set && cursor_ == line_end) {
                if (used >= columns) {
                    l.rows.push_back({row_start, line_end});
                    row_start = line_end;
                    used = 0;
                }
                l.cursor_row = static_cast<int>(l.rows.size());
                l.cursor_column = used;
                cursor_set = true;
            }
            l.rows.push_back({row_start, line_end});
            if (nl == std::string::npos) break;
            line_start = nl + 1;
        }
        return l;
    }

private:
    std::string text_;
    std::size_t cursor_ = 0;
    std::vector<std::string> history_;
    std::size_t index_ = 0;
    std::string stash_;
    bool browsing_ = false;
};

} // namespace lay::ui
