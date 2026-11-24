#pragma once

#include "core/tool_access.hpp"
#include <vector>
#include <map>
#include <memory>
#include <functional>
#include <future>
#include <chrono>
#include <mutex>

namespace ode {

/**
 * 智能工具调度器
 * 超越 Pi 的简单并行/串行执行
 * 自动分析依赖，最优并行度
 */
class SmartToolScheduler {
public:
    struct ToolCall {
        std::string name;
        std::string input;
        ToolAccess access;

        // 依赖信息
        std::vector<std::string> reads_from;   // 读取哪些资源
        std::vector<std::string> writes_to;    // 写入哪些资源
    };

    struct ToolResult {
        std::string output;
        bool success = false;
        std::string error;
        std::chrono::milliseconds elapsed{0};
        size_t memory_used = 0;
        size_t output_size = 0;
    };

    using ExecutorFunc = std::function<ToolResult(const ToolCall&)>;

    // 调度策略
    enum class Strategy {
        Sequential,      // 串行（Pi 有）
        Parallel,        // 简单并行（Pi 有）
        Smart           // 智能调度（Ode 独有！）
    };

    SmartToolScheduler(ExecutorFunc executor, Strategy strategy = Strategy::Smart)
        : executor_(std::move(executor)), strategy_(strategy) {}

    // 执行工具调用列表
    std::vector<ToolResult> execute(const std::vector<ToolCall>& calls);

private:
    // 分析工具依赖
    struct Dependency {
        int tool_index;
        std::vector<int> depends_on;  // 依赖哪些工具的索引
    };

    std::vector<Dependency> analyze_dependencies(const std::vector<ToolCall>& calls);

    // 拓扑排序，返回执行批次
    std::vector<std::vector<int>> topological_sort(
        const std::vector<Dependency>& deps
    );

    // 检查两个工具是否有冲突
    bool has_conflict(const ToolCall& a, const ToolCall& b);

    // 执行一批工具（并行）
    std::vector<ToolResult> execute_batch(
        const std::vector<ToolCall>& calls,
        const std::vector<int>& indices
    );

    ExecutorFunc executor_;
    Strategy strategy_;
};

/**
 * 工具进度追踪
 * Pi 只有简单 spinner，我们提供详细进度
 */
class ToolProgressTracker {
public:
    enum class Status {
        Queued,
        Running,
        Completed,
        Failed,
        Timeout
    };

    struct Progress {
        std::string tool_name;
        Status status = Status::Queued;
        int percent = 0;              // 0-100
        std::string current_step;     // "Reading file..."

        std::chrono::milliseconds elapsed{0};
        std::chrono::milliseconds estimated_remaining{0};

        // 资源使用
        size_t bytes_read = 0;
        size_t bytes_written = 0;
        int cpu_percent = 0;
        size_t memory_kb = 0;
    };

    using ProgressCallback = std::function<void(const Progress&)>;

    void set_callback(ProgressCallback cb) { callback_ = std::move(cb); }

    void start(const std::string& tool_name);
    void update(const std::string& tool_name, int percent, const std::string& step);
    void complete(const std::string& tool_name, bool success);

    Progress get(const std::string& tool_name) const;
    std::vector<Progress> all() const;

private:
    std::map<std::string, Progress> progress_;
    ProgressCallback callback_;
    mutable std::mutex mutex_;
};

} // namespace ode
