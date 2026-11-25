#include "app/command_history.hpp"
#include "platform/file_lock.hpp"

#include <algorithm>
#include <cctype>
#include <fstream>

namespace ode {

command_history::command_history() {
    reset_position();
}

bool command_history::should_add(std::string_view command) const {
    // 不添加空命令
    if (command.empty()) return false;

    // 不添加纯空白命令
    if (std::ranges::all_of(command, [](unsigned char c) { return std::isspace(c); })) return false;

    // 不添加过长的命令
    if (command.size() > max_command_length) return false;

    // 不添加与最近一条相同的命令（避免连续重复）
    if (!history_.empty() && history_.back() == command) return false;

    return true;
}

void command_history::add(std::string_view command) {
    if (!should_add(command)) return;
    remember(pending_, command);
    remember(history_, command);
    reset_position();
}

void command_history::remember(std::deque<std::string>& entries, std::string_view command) {
    const auto existing = std::ranges::find(entries, command);
    if (existing != entries.end()) entries.erase(existing);
    entries.emplace_back(command);
    if (entries.size() > max_history_size) entries.pop_front();
}

void command_history::reset_position() {
    position_ = 0;
    draft_.clear();
}

std::string command_history::previous() {
    if (history_.empty()) return {};

    // 第一次按↑时，保存当前草稿
    if (position_ == 0) {
        draft_ = current();
    }

    // 向历史深处移动
    if (position_ < history_.size()) {
        ++position_;
    }

    return current();
}

std::string command_history::next() {
    if (history_.empty() || position_ == 0) return {};

    // 向最新方向移动
    --position_;

    // 如果回到起点，返回草稿
    if (position_ == 0) {
        std::string result = draft_;
        draft_.clear();
        return result;
    }

    return current();
}

std::string command_history::current() const {
    if (history_.empty() || position_ == 0) {
        return draft_;
    }

    // position_ = 1 表示最后一条命令
    // position_ = 2 表示倒数第二条命令
    // ...
    if (position_ > history_.size()) {
        return history_.front();
    }

    return history_[history_.size() - position_];
}

std::string command_history::search(std::string_view prefix) {
    if (prefix.empty() || history_.empty()) return {};

    // 从最新到最旧搜索匹配前缀的命令
    for (auto it = history_.rbegin(); it != history_.rend(); ++it) {
        if (it->starts_with(prefix)) {
            return *it;
        }
    }

    return {};
}

void command_history::clear() {
    history_.clear();
    pending_.clear();
    cleared_ = true;
    reset_position();
}

void command_history::save(const fs::path& path) {
    if (pending_.empty() && !cleared_) return;
    auto lock_path = path;
    lock_path += L".lock";
    const file_lock lock(lock_path);
    if (!lock) return;

    // Another workspace may have saved history since this instance loaded it.
    command_history merged;
    if (!cleared_) merged.read(path);
    for (const auto& command : pending_) remember(merged.history_, command);

    auto temporary = path;
    temporary += L".tmp";
    std::ofstream file(temporary);
    if (!file) return;

    for (const auto& cmd : merged.history_) {
        // 转义换行符
        std::string escaped;
        for (char c : cmd) {
            if (c == '\n') {
                escaped += "\\n";
            } else if (c == '\\') {
                escaped += "\\\\";
            } else {
                escaped += c;
            }
        }
        file << escaped << '\n';
    }
    file.close();
    std::error_code error;
    if (file) {
        fs::rename(temporary, path, error);
        if (!error) {
            pending_.clear();
            cleared_ = false;
            return;
        }
    }
    fs::remove(temporary, error);
}

void command_history::load(const fs::path& path) {
    auto lock_path = path;
    lock_path += L".lock";
    const file_lock lock(lock_path);
    if (!lock) return;
    read(path);
    pending_.clear();
    cleared_ = false;
}

void command_history::read(const fs::path& path) {
    std::ifstream file(path);
    if (!file) return;

    history_.clear();
    std::string line;

    while (std::getline(file, line)) {
        // 反转义
        std::string unescaped;
        for (size_t i = 0; i < line.size(); ++i) {
            if (line[i] == '\\' && i + 1 < line.size()) {
                if (line[i + 1] == 'n') {
                    unescaped += '\n';
                    ++i;
                } else if (line[i + 1] == '\\') {
                    unescaped += '\\';
                    ++i;
                } else {
                    unescaped += line[i];
                }
            } else {
                unescaped += line[i];
            }
        }

        if (!unescaped.empty() && unescaped.size() <= max_command_length) {
            history_.push_back(std::move(unescaped));
        }

        // 限制加载的历史大小
        if (history_.size() >= max_history_size) break;
    }

    reset_position();
}

} // namespace ode
