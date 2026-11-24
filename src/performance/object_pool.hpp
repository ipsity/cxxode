#pragma once

#include <cstddef>
#include <memory>
#include <vector>
#include <mutex>

namespace ode {
namespace perf {

/**
 * 通用对象池
 * Week 3 Day 15: 性能优化 - 减少内存分配
 *
 * 功能：
 * - 对象重用（避免频繁 new/delete）
 * - 线程安全（可选）
 * - 自动扩展
 *
 * 用法：
 *   object_pool<std::string> pool(100);
 *   auto ptr = pool.acquire();
 *   // 使用 ptr
 *   pool.release(std::move(ptr));
 */
template<typename T>
class object_pool {
public:
    explicit object_pool(size_t initial_size = 32, bool thread_safe = false)
        : thread_safe_(thread_safe) {
        pool_.reserve(initial_size);
        for (size_t i = 0; i < initial_size; ++i) {
            pool_.push_back(std::make_unique<T>());
        }
    }

    // 获取对象（如果池为空则创建新对象）
    std::unique_ptr<T> acquire() {
        if (thread_safe_) {
            std::lock_guard<std::mutex> lock(mutex_);
            return acquire_impl();
        }
        return acquire_impl();
    }

    // 归还对象到池中
    void release(std::unique_ptr<T> obj) {
        if (!obj) return;

        // 重置对象状态（如果 T 有 clear() 方法）
        if constexpr (requires { obj->clear(); }) {
            obj->clear();
        }

        if (thread_safe_) {
            std::lock_guard<std::mutex> lock(mutex_);
            release_impl(std::move(obj));
        } else {
            release_impl(std::move(obj));
        }
    }

    // 获取池中可用对象数量
    size_t available() const {
        if (thread_safe_) {
            std::lock_guard<std::mutex> lock(mutex_);
            return pool_.size();
        }
        return pool_.size();
    }

    // 预分配对象
    void preallocate(size_t count) {
        if (thread_safe_) {
            std::lock_guard<std::mutex> lock(mutex_);
            preallocate_impl(count);
        } else {
            preallocate_impl(count);
        }
    }

    // 清空池
    void clear() {
        if (thread_safe_) {
            std::lock_guard<std::mutex> lock(mutex_);
            pool_.clear();
        } else {
            pool_.clear();
        }
    }

private:
    std::unique_ptr<T> acquire_impl() {
        if (pool_.empty()) {
            return std::make_unique<T>();
        }
        auto obj = std::move(pool_.back());
        pool_.pop_back();
        return obj;
    }

    void release_impl(std::unique_ptr<T> obj) {
        // 限制池大小，避免无限增长
        static constexpr size_t max_pool_size = 1000;
        if (pool_.size() < max_pool_size) {
            pool_.push_back(std::move(obj));
        }
        // else: 让对象被销毁
    }

    void preallocate_impl(size_t count) {
        pool_.reserve(pool_.size() + count);
        for (size_t i = 0; i < count; ++i) {
            pool_.push_back(std::make_unique<T>());
        }
    }

    std::vector<std::unique_ptr<T>> pool_;
    bool thread_safe_;
    mutable std::mutex mutex_;
};

/**
 * 简单的字符串池
 * 专门优化字符串分配
 */
class string_pool {
public:
    explicit string_pool(size_t initial_size = 32)
        : pool_(initial_size) {}

    std::unique_ptr<std::string> acquire() {
        return pool_.acquire();
    }

    void release(std::unique_ptr<std::string> str) {
        pool_.release(std::move(str));
    }

    size_t available() const {
        return pool_.available();
    }

private:
    object_pool<std::string> pool_;
};

} // namespace perf
} // namespace ode
