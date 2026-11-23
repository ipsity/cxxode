#pragma once

#include <chrono>
#include <cstdio>
#include <cstdlib>

namespace lay {

class BenchmarkTimer {
public:
    void record(const char* operation, std::size_t tasks, std::size_t bytes = 0) const {
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start_).count();
#ifdef _WIN32
        const auto* path = _wgetenv(L"LAY_BENCHMARK_TRACE");
        auto* output = path && *path ? _wfopen(path, L"a") : nullptr;
#else
        const auto* path = std::getenv("LAY_BENCHMARK_TRACE");
        auto* output = path && *path ? std::fopen(path, "a") : nullptr;
#endif
        if (output) {
            std::fprintf(output, "%s\t%zu\t%zu\t%lld\n", operation, tasks, bytes, static_cast<long long>(elapsed));
            std::fclose(output);
        }
    }

private:
    using Clock = std::chrono::steady_clock;
    Clock::time_point start_ = Clock::now();
};

} // namespace lay
