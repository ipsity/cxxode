#pragma once

#include <vector>
#include <cstddef>
#include <memory>

namespace ode {
namespace perf {

/**
 * 预分配缓冲区
 * Week 3 Day 19: 性能优化 - 内存管理
 */
template<typename T>
class preallocated_buffer {
public:
    explicit preallocated_buffer(size_t capacity) {
        buffer_.reserve(capacity);
    }

    void push_back(const T& value) { buffer_.push_back(value); }
    void push_back(T&& value) { buffer_.push_back(std::move(value)); }

    template<typename... Args>
    void emplace_back(Args&&... args) {
        buffer_.emplace_back(std::forward<Args>(args)...);
    }

    void clear() { buffer_.clear(); }
    void reset() { buffer_.clear(); buffer_.shrink_to_fit(); }

    const T& operator[](size_t i) const { return buffer_[i]; }
    T& operator[](size_t i) { return buffer_[i]; }

    size_t size() const { return buffer_.size(); }
    size_t capacity() const { return buffer_.capacity(); }
    bool empty() const { return buffer_.empty(); }

    auto begin() { return buffer_.begin(); }
    auto end() { return buffer_.end(); }
    auto begin() const { return buffer_.begin(); }
    auto end() const { return buffer_.end(); }

private:
    std::vector<T> buffer_;
};

/**
 * 内存使用统计
 */
struct memory_stats {
    size_t allocated_bytes = 0;
    size_t deallocated_bytes = 0;
    size_t current_usage = 0;
    size_t peak_usage = 0;
    size_t allocation_count = 0;

    void record_alloc(size_t bytes) {
        allocated_bytes += bytes;
        current_usage += bytes;
        allocation_count++;
        if (current_usage > peak_usage) {
            peak_usage = current_usage;
        }
    }

    void record_dealloc(size_t bytes) {
        deallocated_bytes += bytes;
        if (current_usage >= bytes) {
            current_usage -= bytes;
        }
    }

    void reset() {
        allocated_bytes = deallocated_bytes = 0;
        current_usage = peak_usage = 0;
        allocation_count = 0;
    }
};

/**
 * 全局内存统计
 */
inline memory_stats& global_memory_stats() {
    static memory_stats stats;
    return stats;
}

} // namespace perf
} // namespace ode
