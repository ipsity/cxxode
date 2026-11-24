#pragma once

namespace ode {
namespace perf {

/**
 * 性能配置
 * Week 3 Day 20: 性能优化 - 运行时配置
 */
struct performance_config {
    // 对象池配置
    bool enable_object_pools = true;
    size_t default_pool_size = 32;

    // 缓存配置
    bool enable_caching = true;
    size_t default_cache_size = 100;

    // 线程池配置
    size_t thread_pool_size = 0;  // 0 = auto (hardware_concurrency)

    // 性能监控
    bool enable_metrics = false;
    bool enable_timing = false;

    // 内存优化
    bool enable_memory_pooling = true;
    bool enable_string_pooling = true;

    // 获取全局配置
    static performance_config& global() {
        static performance_config config;
        return config;
    }
};

} // namespace perf
} // namespace ode
