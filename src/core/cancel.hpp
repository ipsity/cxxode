#pragma once

#include <atomic>
#include <functional>
#include <mutex>

namespace ode {

// Shared between the UI thread (cancel) and the worker (hooks). A hook lets a
// blocking call such as an HTTP read be interrupted from the outside.
class cancel_token {
public:
    void cancel() {
        flag_.store(true);
        std::lock_guard lock(mutex_);
        if (hook_) {
            auto hook = std::move(hook_);
            hook_ = nullptr;
            hook();
        }
    }
    bool cancelled() const { return flag_.load(); }
    void reset() { flag_.store(false); }

    // Returns false if already cancelled; the hook is then not installed.
    bool set_hook(std::function<void()> hook) {
        std::lock_guard lock(mutex_);
        if (flag_.load()) return false;
        hook_ = std::move(hook);
        return true;
    }
    // Returns true if the hook was still installed, i.e. it never ran.
    bool clear_hook() {
        std::lock_guard lock(mutex_);
        const bool pending = static_cast<bool>(hook_);
        hook_ = nullptr;
        return pending;
    }

private:
    std::atomic<bool> flag_{false};
    std::mutex mutex_;
    std::function<void()> hook_;
};

} // namespace ode
