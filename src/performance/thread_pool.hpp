#pragma once

#include <vector>
#include <queue>
#include <thread>
#include <mutex>
#include <condition_variable>
#include <functional>
#include <future>
#include <atomic>

namespace ode {
namespace perf {

/**
 * 线程池
 * Week 3 Day 18: 性能优化 - 并发执行
 *
 * 功能：
 * - 固定大小的线程池
 * - 任务队列
 * - 异步任务提交
 * - Future 支持
 */
class thread_pool {
public:
    explicit thread_pool(size_t thread_count = std::thread::hardware_concurrency())
        : stop_(false) {
        if (thread_count == 0) thread_count = 1;

        workers_.reserve(thread_count);
        for (size_t i = 0; i < thread_count; ++i) {
            workers_.emplace_back([this] { worker_thread(); });
        }
    }

    ~thread_pool() {
        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            stop_ = true;
        }
        condition_.notify_all();

        for (auto& worker : workers_) {
            if (worker.joinable()) {
                worker.join();
            }
        }
    }

    // 禁止拷贝和移动
    thread_pool(const thread_pool&) = delete;
    thread_pool& operator=(const thread_pool&) = delete;

    // 提交任务并返回 future
    template<typename F, typename... Args>
    auto submit(F&& f, Args&&... args) -> std::future<typename std::invoke_result<F, Args...>::type> {
        using return_type = typename std::invoke_result<F, Args...>::type;

        auto task = std::make_shared<std::packaged_task<return_type()>>(
            std::bind(std::forward<F>(f), std::forward<Args>(args)...)
        );

        std::future<return_type> result = task->get_future();

        {
            std::unique_lock<std::mutex> lock(queue_mutex_);
            if (stop_) {
                throw std::runtime_error("Cannot submit to stopped thread pool");
            }
            tasks_.emplace([task]() { (*task)(); });
        }

        condition_.notify_one();
        return result;
    }

    // 获取线程数量
    size_t thread_count() const { return workers_.size(); }

    // 获取队列中的任务数量
    size_t queued_tasks() const {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        return tasks_.size();
    }

    // 等待所有任务完成
    void wait_all() {
        std::unique_lock<std::mutex> lock(queue_mutex_);
        condition_.wait(lock, [this] {
            return tasks_.empty() && active_tasks_ == 0;
        });
    }

private:
    void worker_thread() {
        while (true) {
            std::function<void()> task;

            {
                std::unique_lock<std::mutex> lock(queue_mutex_);
                condition_.wait(lock, [this] {
                    return stop_ || !tasks_.empty();
                });

                if (stop_ && tasks_.empty()) {
                    return;
                }

                if (!tasks_.empty()) {
                    task = std::move(tasks_.front());
                    tasks_.pop();
                    ++active_tasks_;
                }
            }

            if (task) {
                task();
                --active_tasks_;
                condition_.notify_all();
            }
        }
    }

    std::vector<std::thread> workers_;
    std::queue<std::function<void()>> tasks_;
    mutable std::mutex queue_mutex_;
    std::condition_variable condition_;
    std::atomic<bool> stop_;
    std::atomic<size_t> active_tasks_{0};
};

/**
 * 全局线程池
 */
inline thread_pool& global_thread_pool() {
    static thread_pool pool;
    return pool;
}

} // namespace perf
} // namespace ode
