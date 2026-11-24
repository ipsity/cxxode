#include "core/tool_scheduler.hpp"
#include <algorithm>
#include <unordered_set>
#include <queue>

namespace ode {

std::vector<SmartToolScheduler::ToolResult>
SmartToolScheduler::execute(const std::vector<ToolCall>& calls) {
    if (calls.empty()) return {};

    std::vector<ToolResult> results(calls.size());

    // 检查 steering（用户实时引导）
    if (steering_) {
        auto messages = steering_();
        if (!messages.empty()) {
            // 用户有新指令，可能需要调整执行
            // TODO: 处理 steering 消息
        }
    }

    switch (strategy_) {
        case Strategy::Sequential: {
            // 串行执行
            for (size_t i = 0; i < calls.size(); ++i) {
                results[i] = executor_(calls[i]);
            }
            break;
        }

        case Strategy::Parallel: {
            // 简单并行（Pi 的方式）
            std::vector<std::future<ToolResult>> futures;
            for (const auto& call : calls) {
                futures.push_back(std::async(std::launch::async,
                    executor_, call));
            }
            for (size_t i = 0; i < futures.size(); ++i) {
                results[i] = futures[i].get();
            }
            break;
        }

        case Strategy::Smart: {
            // 智能调度（Ode 独有！）
            auto deps = analyze_dependencies(calls);
            auto stages = topological_sort(deps);

            // 按阶段执行，每阶段内并行
            for (const auto& stage_indices : stages) {
                auto stage_results = execute_batch(calls, stage_indices);
                for (size_t i = 0; i < stage_indices.size(); ++i) {
                    results[stage_indices[i]] = stage_results[i];
                }
            }
            break;
        }
    }

    return results;
}

std::vector<SmartToolScheduler::Dependency>
SmartToolScheduler::analyze_dependencies(const std::vector<ToolCall>& calls) {
    std::vector<Dependency> deps;
    deps.reserve(calls.size());

    for (size_t i = 0; i < calls.size(); ++i) {
        Dependency dep;
        dep.tool_index = i;

        // 检查与之前工具的依赖
        for (size_t j = 0; j < i; ++j) {
            if (has_conflict(calls[i], calls[j])) {
                dep.depends_on.push_back(j);
            }
        }

        deps.push_back(dep);
    }

    return deps;
}

bool SmartToolScheduler::has_conflict(const ToolCall& a, const ToolCall& b) {
    // 检查读写冲突
    // Write-after-write
    for (const auto& a_write : a.writes_to) {
        for (const auto& b_write : b.writes_to) {
            if (a_write == b_write) return true;
        }
    }

    // Read-after-write
    for (const auto& a_read : a.reads_from) {
        for (const auto& b_write : b.writes_to) {
            if (a_read == b_write) return true;
        }
    }

    // Write-after-read
    for (const auto& a_write : a.writes_to) {
        for (const auto& b_read : b.reads_from) {
            if (a_write == b_read) return true;
        }
    }

    return false;
}

std::vector<std::vector<int>>
SmartToolScheduler::topological_sort(const std::vector<Dependency>& deps) {
    std::vector<std::vector<int>> stages;
    std::unordered_set<int> completed;

    while (completed.size() < deps.size()) {
        std::vector<int> current_stage;

        // 找出所有依赖已满足的工具
        for (const auto& dep : deps) {
            if (completed.count(dep.tool_index)) continue;

            bool ready = true;
            for (int depends : dep.depends_on) {
                if (!completed.count(depends)) {
                    ready = false;
                    break;
                }
            }

            if (ready) {
                current_stage.push_back(dep.tool_index);
            }
        }

        if (current_stage.empty()) {
            // 循环依赖，强制执行剩余的
            for (const auto& dep : deps) {
                if (!completed.count(dep.tool_index)) {
                    current_stage.push_back(dep.tool_index);
                }
            }
        }

        for (int idx : current_stage) {
            completed.insert(idx);
        }

        stages.push_back(current_stage);
    }

    return stages;
}

std::vector<SmartToolScheduler::ToolResult>
SmartToolScheduler::execute_batch(
    const std::vector<ToolCall>& calls,
    const std::vector<int>& indices
) {
    std::vector<std::future<ToolResult>> futures;

    for (int idx : indices) {
        futures.push_back(std::async(std::launch::async,
            executor_, calls[idx]));
    }

    std::vector<ToolResult> results;
    for (auto& future : futures) {
        results.push_back(future.get());
    }

    return results;
}

// ToolProgressTracker 实现

void ToolProgressTracker::start(const std::string& tool_name) {
    std::lock_guard lock(mutex_);
    Progress& p = progress_[tool_name];
    p.tool_name = tool_name;
    p.status = Status::Running;
    p.percent = 0;

    if (callback_) callback_(p);
}

void ToolProgressTracker::update(
    const std::string& tool_name,
    int percent,
    const std::string& step
) {
    std::lock_guard lock(mutex_);
    Progress& p = progress_[tool_name];
    p.percent = percent;
    p.current_step = step;

    if (callback_) callback_(p);
}

void ToolProgressTracker::complete(const std::string& tool_name, bool success) {
    std::lock_guard lock(mutex_);
    Progress& p = progress_[tool_name];
    p.status = success ? Status::Completed : Status::Failed;
    p.percent = 100;

    if (callback_) callback_(p);
}

ToolProgressTracker::Progress
ToolProgressTracker::get(const std::string& tool_name) const {
    std::lock_guard lock(mutex_);
    auto it = progress_.find(tool_name);
    return it != progress_.end() ? it->second : Progress{};
}

std::vector<ToolProgressTracker::Progress>
ToolProgressTracker::all() const {
    std::lock_guard lock(mutex_);
    std::vector<Progress> result;
    for (const auto& [_, p] : progress_) {
        result.push_back(p);
    }
    return result;
}

} // namespace ode
