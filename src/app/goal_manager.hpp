#pragma once

#include <string>
#include <chrono>
#include <optional>

namespace ode {

/**
 * Goal 管理系统
 * 类似 Pi 的 Goal 功能 - 设置工作目标，AI 持续工作直到完成
 */
class goal_manager {
public:
    struct goal {
        std::string description;           // Goal 描述
        std::chrono::steady_clock::time_point created_at;  // 创建时间
        bool is_active = true;             // 是否激活

        goal(std::string desc)
            : description(std::move(desc))
            , created_at(std::chrono::steady_clock::now()) {}
    };

    goal_manager() = default;

    // 设置新的 goal
    void set_goal(std::string description) {
        current_goal_ = goal(std::move(description));
    }

    // 获取当前 goal
    std::optional<goal> current_goal() const {
        return current_goal_;
    }

    // 清除 goal
    void clear_goal() {
        current_goal_.reset();
    }

    // 检查是否有活跃的 goal
    bool has_active_goal() const {
        return current_goal_.has_value() && current_goal_->is_active;
    }

    // 获取 goal 运行时间（秒）
    int64_t goal_duration_seconds() const {
        if (!current_goal_) return 0;
        auto now = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::seconds>(
            now - current_goal_->created_at);
        return duration.count();
    }

    // 格式化显示 goal 状态
    std::string format_status() const {
        if (!current_goal_) return "";

        auto duration = goal_duration_seconds();
        int hours = duration / 3600;
        int mins = (duration % 3600) / 60;
        int secs = duration % 60;

        std::string time_str;
        if (hours > 0) {
            time_str = std::to_string(hours) + "h " + std::to_string(mins) + "m";
        } else if (mins > 0) {
            time_str = std::to_string(mins) + "m " + std::to_string(secs) + "s";
        } else {
            time_str = std::to_string(secs) + "s";
        }

        return "🎯 Goal: " + current_goal_->description + " (" + time_str + ")";
    }

private:
    std::optional<goal> current_goal_;
};

} // namespace ode
