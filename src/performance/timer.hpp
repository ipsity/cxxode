#pragma once

#include <chrono>
#include <string>
#include <string_view>
#include <functional>

namespace ode {
namespace perf {

/**
 * 高精度计时器
 * Week 3 Day 15: 性能优化 - 性能监控基础
 *
 * 功能：
 * - 高精度时间测量
 * - 自动计时（RAII）
 * - 性能统计收集
 */
class timer {
public:
    using clock = std::chrono::high_resolution_clock;
    using time_point = clock::time_point;
    using duration = clock::duration;

    // 开始计时
    void start() { start_ = clock::now(); }

    // 停止并返回经过的时间（毫秒）
    double stop() {
        end_ = clock::now();
        return elapsed_ms();
    }

    // 重置计时器
    void reset() { start_ = end_ = clock::now(); }

    // 获取经过的时间（不停止）
    double elapsed_ms() const {
        auto end = end_.time_since_epoch().count() ? end_ : clock::now();
        return std::chrono::duration<double, std::milli>(end - start_).count();
    }

    double elapsed_us() const {
        auto end = end_.time_since_epoch().count() ? end_ : clock::now();
        return std::chrono::duration<double, std::micro>(end - start_).count();
    }

    double elapsed_seconds() const {
        return elapsed_ms() / 1000.0;
    }

private:
    time_point start_ = clock::now();
    time_point end_{};
};

/**
 * 自动计时器（RAII）
 * 在作用域结束时自动记录时间
 */
class scoped_timer {
public:
    using callback_t = std::function<void(double)>;

    explicit scoped_timer(std::string_view name, callback_t callback = {})
        : name_(name), callback_(std::move(callback)) {
        timer_.start();
    }

    ~scoped_timer() {
        double elapsed = timer_.stop();
        if (callback_) {
            callback_(elapsed);
        }
    }

    // 获取当前经过的时间（不结束计时）
    double elapsed_ms() const { return timer_.elapsed_ms(); }

private:
    std::string name_;
    timer timer_;
    callback_t callback_;
};

} // namespace perf
} // namespace ode
