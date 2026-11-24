#pragma once

#include <deque>
#include <string>
#include <string_view>
#include <filesystem>

namespace ode {

namespace fs = std::filesystem;

/**
 * 命令历史管理器
 * Week 2 Day 10: 交互优化 - 命令历史浏览
 *
 * 功能：
 * - 存储用户输入历史
 * - 支持 ↑/↓ 键浏览
 * - 持久化到文件
 * - 自动去重
 * - 限制历史大小
 */
class command_history {
public:
    command_history();

    // 添加命令到历史（自动去重）
    void add(std::string_view command);

    // 浏览历史：获取上一条命令
    std::string previous();

    // 浏览历史：获取下一条命令
    std::string next();

    // 重置浏览位置到最新
    void reset_position();

    // 获取当前浏览位置的命令
    std::string current() const;

    // 搜索历史（前缀匹配）
    std::string search(std::string_view prefix);

    // 获取历史大小
    size_t size() const { return history_.size(); }

    // 清空历史
    void clear();

    // 持久化
    void save(const fs::path& path) const;
    void load(const fs::path& path);

private:
    std::deque<std::string> history_;
    size_t position_ = 0;              // 当前浏览位置（0 = 最新）
    std::string draft_;                // 用户正在输入的草稿（浏览历史时保存）

    static constexpr size_t max_history_size = 1000;
    static constexpr size_t max_command_length = 10000;

    // 判断命令是否应该添加到历史
    bool should_add(std::string_view command) const;
};

} // namespace ode
