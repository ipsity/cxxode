#include "core/command_history.hpp"
#include <algorithm>
#include <fstream>

namespace ode {

command_history::command_history() {
    reset_position();
}

bool command_history::should_add(std::string_view command) const {
    // 不添加空命令
    if (command.empty()) return false;

    // 不添加纯空白命令
    if (std::all_of(command.begin(), command.end(), ::isspace)) return false;

    // 不添加过长的命令
    if (command.size() > max_command_length) return false;

    // 不添加与最近一条相同的命令（避免连续重复）
    if (!history_.empty() && history_.back() == command) return false;

    return true;
}

void command_history::add(std::string_view command) {
    if (!should_add(command)) return;

    // 移除历史中的重复项（保持最新的位置）
    auto it = std::find(history_.begin(), history_.end(), command);
    if (it != history_.end()) {
        history_.erase(it);
    }

    // 添加到历史末尾（最新）
    history_.push_back(std::string(command));

    // 限制历史大小
    if (history_.size() > max_history_size) {
        history_.pop_front();
    }

    // 重置浏览位置
    reset_position();
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
    reset_position();
}

void command_history::save(const fs::path& path) const {
    std::ofstream file(path);
    if (!file) return;

    // 保存最近的命令（最多 max_history_size 条）
    for (const auto& cmd : history_) {
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
}

void command_history::load(const fs::path& path) {
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
