#pragma once

#include <unordered_map>
#include <list>
#include <optional>
#include <mutex>
#include <functional>

namespace ode {
namespace perf {

/**
 * LRU (Least Recently Used) 缓存
 * Week 3 Day 17: 性能优化 - 缓存系统
 *
 * 功能：
 * - 固定大小的缓存
 * - LRU 淘汰策略
 * - 线程安全（可选）
 * - O(1) 查找和更新
 *
 * 用法：
 *   lru_cache<std::string, int> cache(100);
 *   cache.put("key", 42);
 *   auto value = cache.get("key");  // returns std::optional<int>
 */
template<typename Key, typename Value>
class lru_cache {
public:
    using key_type = Key;
    using value_type = Value;
    using list_iterator = typename std::list<std::pair<Key, Value>>::iterator;

    explicit lru_cache(size_t capacity, bool thread_safe = false)
        : capacity_(capacity), thread_safe_(thread_safe) {
        if (capacity_ == 0) capacity_ = 1;
    }

    // 插入或更新键值对
    void put(const Key& key, const Value& value) {
        if (thread_safe_) {
            std::lock_guard<std::mutex> lock(mutex_);
            put_impl(key, value);
        } else {
            put_impl(key, value);
        }
    }

    void put(Key&& key, Value&& value) {
        if (thread_safe_) {
            std::lock_guard<std::mutex> lock(mutex_);
            put_impl(std::move(key), std::move(value));
        } else {
            put_impl(std::move(key), std::move(value));
        }
    }

    // 获取值（如果存在）
    std::optional<Value> get(const Key& key) {
        if (thread_safe_) {
            std::lock_guard<std::mutex> lock(mutex_);
            return get_impl(key);
        }
        return get_impl(key);
    }

    // 检查键是否存在
    bool contains(const Key& key) const {
        if (thread_safe_) {
            std::lock_guard<std::mutex> lock(mutex_);
            return map_.find(key) != map_.end();
        }
        return map_.find(key) != map_.end();
    }

    // 删除键
    void erase(const Key& key) {
        if (thread_safe_) {
            std::lock_guard<std::mutex> lock(mutex_);
            erase_impl(key);
        } else {
            erase_impl(key);
        }
    }

    // 清空缓存
    void clear() {
        if (thread_safe_) {
            std::lock_guard<std::mutex> lock(mutex_);
            list_.clear();
            map_.clear();
        } else {
            list_.clear();
            map_.clear();
        }
    }

    // 获取当前大小
    size_t size() const {
        if (thread_safe_) {
            std::lock_guard<std::mutex> lock(mutex_);
            return map_.size();
        }
        return map_.size();
    }

    // 获取容量
    size_t capacity() const { return capacity_; }

    // 是否为空
    bool empty() const { return size() == 0; }

    // 是否已满
    bool full() const { return size() >= capacity_; }

private:
    void put_impl(const Key& key, const Value& value) {
        auto it = map_.find(key);
        if (it != map_.end()) {
            // 更新现有项
            list_.erase(it->second);
            map_.erase(it);
        } else if (map_.size() >= capacity_) {
            // 淘汰最久未使用的项
            auto& oldest = list_.back();
            map_.erase(oldest.first);
            list_.pop_back();
        }

        // 插入新项到最前面
        list_.push_front({key, value});
        map_[key] = list_.begin();
    }

    void put_impl(Key&& key, Value&& value) {
        auto it = map_.find(key);
        if (it != map_.end()) {
            list_.erase(it->second);
            map_.erase(it);
        } else if (map_.size() >= capacity_) {
            auto& oldest = list_.back();
            map_.erase(oldest.first);
            list_.pop_back();
        }

        list_.push_front({std::move(key), std::move(value)});
        map_[list_.front().first] = list_.begin();
    }

    std::optional<Value> get_impl(const Key& key) {
        auto it = map_.find(key);
        if (it == map_.end()) {
            return std::nullopt;
        }

        // 移动到最前面（最近使用）
        list_.splice(list_.begin(), list_, it->second);
        return it->second->second;
    }

    void erase_impl(const Key& key) {
        auto it = map_.find(key);
        if (it != map_.end()) {
            list_.erase(it->second);
            map_.erase(it);
        }
    }

    size_t capacity_;
    bool thread_safe_;
    mutable std::mutex mutex_;
    std::list<std::pair<Key, Value>> list_;  // MRU at front, LRU at back
    std::unordered_map<Key, list_iterator> map_;
};

/**
 * 结果缓存装饰器
 * 自动缓存函数结果
 */
template<typename Result, typename... Args>
class result_cache {
public:
    using function_type = std::function<Result(Args...)>;
    using key_type = std::tuple<Args...>;

    explicit result_cache(function_type func, size_t capacity = 100)
        : func_(std::move(func)), cache_(capacity, true) {}

    Result operator()(Args... args) {
        key_type key{args...};

        // 尝试从缓存获取
        auto cached = cache_.get(key);
        if (cached) {
            return *cached;
        }

        // 计算结果并缓存
        Result result = func_(args...);
        cache_.put(key, result);
        return result;
    }

    void clear() { cache_.clear(); }
    size_t size() const { return cache_.size(); }

private:
    function_type func_;
    lru_cache<key_type, Result> cache_;
};

} // namespace perf
} // namespace ode
