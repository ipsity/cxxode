#pragma once

#include <string>
#include <string_view>
#include <vector>
#include <deque>

namespace ode {

/**
 * 工具建议引擎
 * Week 2 Day 8: UX 优化 - 智能工具推荐
 *
 * 功能：
 * - 分析用户输入上下文
 * - 识别实体（文件路径、命令、关键词）
 * - 推荐相关工具
 * - 基于历史学习使用模式
 */
class tool_suggestion_engine {
public:
    struct suggestion {
        std::string tool_name;      // 工具名称
        std::string reason;         // 推荐原因
        std::string hint;           // 使用提示（可选）
        float confidence = 0.0f;    // 置信度 0.0-1.0

        bool operator<(const suggestion& other) const {
            return confidence > other.confidence;  // 降序排列
        }
    };

    tool_suggestion_engine() = default;

    // 分析用户输入并推荐工具
    // 识别：文件路径、目录、搜索意图、执行意图、调试意图
    std::vector<suggestion> analyze(std::string_view user_input) const;

    // 记录工具使用历史（用于模式学习）
    void record_tool_use(std::string_view tool_name);

    // 基于最近使用的工具推荐下一步
    // 例如：Read 后推荐 Edit，Edit 后推荐 Write
    std::vector<suggestion> suggest_next(std::string_view last_tool) const;

    // 获取最近使用的工具序列
    std::vector<std::string> recent_tools(size_t count = 5) const;

private:
    // 实体识别
    bool contains_file_path(std::string_view text) const;
    bool contains_directory_path(std::string_view text) const;
    bool contains_search_intent(std::string_view text) const;
    bool contains_execute_intent(std::string_view text) const;
    bool contains_debug_intent(std::string_view text) const;
    bool contains_edit_intent(std::string_view text) const;

    // 提取实体
    std::vector<std::string> extract_file_paths(std::string_view text) const;

    // 历史记录（最近 20 次）
    std::deque<std::string> history_;
    static constexpr size_t max_history_size = 20;
};

} // namespace ode
