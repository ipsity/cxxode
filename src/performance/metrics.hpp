#pragma once

#include <string>
#include <string_view>
#include <map>
#include <vector>
#include <cmath>
#include <algorithm>
#include <mutex>

namespace ode {
namespace perf {

/**
 * 性能指标收集器
 * Week 3 Day 15: 性能优化 - 性能监控基础
 *
 * 功能：
 * - 记录命名的性能指标
 * - 计算统计信息（min/max/avg/stddev）
 * - 线程安全
 * - 可重置
 *
 * 用法：
 *   metrics_collector metrics;
 *   metrics.record("tool_execution", 123.45);
 *   auto stats = metrics.get_stats("tool_execution");
 */
class metrics_collector {
public:
    struct stats {
        double min = 0.0;
        double max = 0.0;
        double avg = 0.0;
        double stddev = 0.0;
        double total = 0.0;
        size_t count = 0;

        bool empty() const { return count == 0; }
    };

    metrics_collector() = default;

    // 记录单个数据点
    void record(std::string_view name, double value) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& data = data_[std::string(name)];
        data.push_back(value);
    }

    // 记录多个数据点
    void record_batch(std::string_view name, const std::vector<double>& values) {
        std::lock_guard<std::mutex> lock(mutex_);
        auto& data = data_[std::string(name)];
        data.insert(data.end(), values.begin(), values.end());
    }

    // 获取统计信息
    stats get_stats(std::string_view name) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = data_.find(std::string(name));
        if (it == data_.end() || it->second.empty()) {
            return {};
        }

        const auto& values = it->second;
        stats result;
        result.count = values.size();

        // 计算 min, max, total
        result.min = *std::min_element(values.begin(), values.end());
        result.max = *std::max_element(values.begin(), values.end());
        result.total = 0.0;
        for (double v : values) {
            result.total += v;
        }

        // 计算平均值
        result.avg = result.total / result.count;

        // 计算标准差
        if (result.count > 1) {
            double variance = 0.0;
            for (double v : values) {
                double diff = v - result.avg;
                variance += diff * diff;
            }
            variance /= result.count;
            result.stddev = std::sqrt(variance);
        }

        return result;
    }

    // 获取所有指标名称
    std::vector<std::string> get_all_names() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> names;
        names.reserve(data_.size());
        for (const auto& [name, _] : data_) {
            names.push_back(name);
        }
        return names;
    }

    // 重置所有指标
    void reset() {
        std::lock_guard<std::mutex> lock(mutex_);
        data_.clear();
    }

    // 重置特定指标
    void reset(std::string_view name) {
        std::lock_guard<std::mutex> lock(mutex_);
        data_.erase(std::string(name));
    }

    // 获取原始数据（用于高级分析）
    std::vector<double> get_raw_data(std::string_view name) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = data_.find(std::string(name));
        if (it == data_.end()) {
            return {};
        }
        return it->second;
    }

    // 获取数据点数量
    size_t count(std::string_view name) const {
        std::lock_guard<std::mutex> lock(mutex_);
        auto it = data_.find(std::string(name));
        if (it == data_.end()) {
            return 0;
        }
        return it->second.size();
    }

    // 是否为空
    bool empty() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return data_.empty();
    }

    // 总数据点数量
    size_t total_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t total = 0;
        for (const auto& [_, values] : data_) {
            total += values.size();
        }
        return total;
    }

private:
    mutable std::mutex mutex_;
    std::map<std::string, std::vector<double>> data_;
};

/**
 * 全局性能指标收集器
 * 方便在应用程序中使用
 */
inline metrics_collector& global_metrics() {
    static metrics_collector instance;
    return instance;
}

/**
 * 便捷的性能测量宏
 * 自动记录到全局收集器
 */
#define PERF_MEASURE(name) \
    ode::perf::scoped_timer _perf_timer_##__LINE__(name, \
        [](double ms) { ode::perf::global_metrics().record(name, ms); })

} // namespace perf
} // namespace ode
