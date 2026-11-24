#pragma once

#include "innovation/pattern_learning.hpp"
#include "core/tool_suggestion.hpp"
#include <string>
#include <vector>
#include <algorithm>

namespace ode {

/**
 * 自适应推荐系统
 * Week 4 Day 25: 创新功能 - 智能推荐
 *
 * 功能：
 * - 结合上下文和历史
 * - 自适应学习
 * - 个性化推荐
 */

struct adaptive_suggestion {
    std::string tool_name;
    std::string reason;
    double score = 0.0;  // 综合评分

    bool operator<(const adaptive_suggestion& other) const {
        return score > other.score;  // 降序
    }
};

class adaptive_recommender {
public:
    adaptive_recommender(tool_suggestion_engine& context_engine,
                        pattern_learner& pattern_engine)
        : context_engine_(context_engine),
          pattern_engine_(pattern_engine) {}

    // 生成自适应推荐
    std::vector<adaptive_suggestion> recommend(
        std::string_view user_input,
        const std::vector<std::string>& recent_tools,
        size_t max_suggestions = 5) {

        std::map<std::string, double> tool_scores;

        // 1. 基于上下文的推荐（60%权重）
        auto context_suggestions = context_engine_.analyze(user_input);
        for (const auto& sugg : context_suggestions) {
            tool_scores[sugg.tool_name] += sugg.confidence * 0.6;
        }

        // 2. 基于模式的推荐（40%权重）
        auto pattern_predictions = pattern_engine_.predict_next(recent_tools);
        double pattern_weight = 0.4 / std::max<size_t>(pattern_predictions.size(), 1);
        for (size_t i = 0; i < pattern_predictions.size(); ++i) {
            double decay = 1.0 - (i * 0.15);  // 递减权重
            tool_scores[pattern_predictions[i]] += pattern_weight * decay;
        }

        // 3. 频率加权（从历史学习）
        if (pattern_engine_.history_size() > 0) {
            auto common_patterns = pattern_engine_.get_common_patterns(10);
            for (const auto& pattern : common_patterns) {
                if (!pattern.sequence.empty()) {
                    const auto& tool = pattern.sequence.back();
                    tool_scores[tool] += pattern.confidence * 0.1;
                }
            }
        }

        // 转换为建议列表
        std::vector<adaptive_suggestion> suggestions;
        for (const auto& [tool, score] : tool_scores) {
            adaptive_suggestion sugg;
            sugg.tool_name = tool;
            sugg.score = score;
            sugg.reason = generate_reason(tool, score);
            suggestions.push_back(sugg);
        }

        // 排序并限制数量
        std::sort(suggestions.begin(), suggestions.end());
        if (suggestions.size() > max_suggestions) {
            suggestions.resize(max_suggestions);
        }

        return suggestions;
    }

    // 记录用户选择（强化学习）
    void record_choice(const std::string& tool_name) {
        pattern_engine_.record(tool_name);
    }

private:
    std::string generate_reason(const std::string& tool_name, double score) {
        if (score > 0.7) {
            return "强烈推荐（基于上下文和历史）";
        } else if (score > 0.5) {
            return "推荐（基于使用模式）";
        } else if (score > 0.3) {
            return "建议尝试";
        } else {
            return "可能相关";
        }
    }

    tool_suggestion_engine& context_engine_;
    pattern_learner& pattern_engine_;
};

/**
 * 推荐增强器
 * 包装现有推荐系统，添加学习能力
 */
class recommendation_enhancer {
public:
    recommendation_enhancer()
        : recommender_(context_engine_, pattern_engine_) {}

    std::vector<adaptive_suggestion> get_suggestions(
        std::string_view input,
        const std::vector<std::string>& recent_tools) {
        return recommender_.recommend(input, recent_tools);
    }

    void learn_from_usage(const std::string& tool_name) {
        recommender_.record_choice(tool_name);
    }

    void clear_history() {
        pattern_engine_.clear();
    }

private:
    tool_suggestion_engine context_engine_;
    pattern_learner pattern_engine_;
    adaptive_recommender recommender_;
};

} // namespace ode
