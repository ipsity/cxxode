#pragma once

#include <string>
#include <vector>
#include <map>
#include <deque>
#include <algorithm>

namespace ode {

/**
 * 模式学习系统
 * Week 4 Day 24: 创新功能 - 使用模式识别
 *
 * 功能：
 * - 记录工具使用序列
 * - 识别常见模式
 * - 生成智能推荐
 */

struct usage_pattern {
    std::vector<std::string> sequence;  // 工具序列
    int frequency = 0;                  // 出现频率
    double confidence = 0.0;            // 置信度

    bool operator<(const usage_pattern& other) const {
        return frequency > other.frequency;  // 降序
    }
};

class pattern_learner {
public:
    explicit pattern_learner(size_t max_history = 1000, size_t pattern_length = 3)
        : max_history_(max_history), pattern_length_(pattern_length) {}

    // 记录工具使用
    void record(const std::string& tool_name) {
        history_.push_back(tool_name);
        if (history_.size() > max_history_) {
            history_.pop_front();
        }
        needs_update_ = true;
    }

    // 获取最常见的模式
    std::vector<usage_pattern> get_common_patterns(size_t top_n = 10) {
        if (needs_update_) {
            update_patterns();
            needs_update_ = false;
        }

        std::vector<usage_pattern> patterns;
        for (const auto& [seq, freq] : pattern_freq_) {
            usage_pattern p;
            p.sequence = seq;
            p.frequency = freq;
            p.confidence = static_cast<double>(freq) / history_.size();
            patterns.push_back(p);
        }

        std::sort(patterns.begin(), patterns.end());

        if (patterns.size() > top_n) {
            patterns.resize(top_n);
        }

        return patterns;
    }

    // 预测下一个工具
    std::vector<std::string> predict_next(const std::vector<std::string>& recent_tools) {
        if (needs_update_) {
            update_patterns();
            needs_update_ = false;
        }

        std::map<std::string, int> next_tool_freq;

        // 在历史中查找匹配的序列
        for (size_t i = 0; i + recent_tools.size() < history_.size(); ++i) {
            bool match = true;
            for (size_t j = 0; j < recent_tools.size(); ++j) {
                if (history_[i + j] != recent_tools[j]) {
                    match = false;
                    break;
                }
            }

            if (match && i + recent_tools.size() < history_.size()) {
                next_tool_freq[history_[i + recent_tools.size()]]++;
            }
        }

        // 转换为排序的列表
        std::vector<std::pair<std::string, int>> sorted_next(
            next_tool_freq.begin(), next_tool_freq.end());

        std::sort(sorted_next.begin(), sorted_next.end(),
                 [](const auto& a, const auto& b) { return a.second > b.second; });

        std::vector<std::string> predictions;
        for (const auto& [tool, _] : sorted_next) {
            predictions.push_back(tool);
            if (predictions.size() >= 5) break;
        }

        return predictions;
    }

    // 获取历史大小
    size_t history_size() const { return history_.size(); }

    // 清空历史
    void clear() {
        history_.clear();
        pattern_freq_.clear();
        needs_update_ = true;
    }

private:
    void update_patterns() {
        pattern_freq_.clear();

        // 提取所有长度为 pattern_length_ 的序列
        for (size_t i = 0; i + pattern_length_ <= history_.size(); ++i) {
            std::vector<std::string> seq(
                history_.begin() + i,
                history_.begin() + i + pattern_length_
            );
            pattern_freq_[seq]++;
        }
    }

    size_t max_history_;
    size_t pattern_length_;
    std::deque<std::string> history_;
    std::map<std::vector<std::string>, int> pattern_freq_;
    bool needs_update_ = false;
};

/**
 * 全局模式学习器
 */
inline pattern_learner& global_pattern_learner() {
    static pattern_learner learner;
    return learner;
}

} // namespace ode
