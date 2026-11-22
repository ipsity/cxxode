#pragma once

#include "core/session.hpp"
#include "platform/workspace.hpp"

#include <array>
#include <chrono>

namespace lay::platform {

struct ProcessInput {
    std::filesystem::path executable;
    const Node& directory;
    const TextList& arguments;
    const TextList& environment;
    std::chrono::milliseconds timeout;
};

TextList child_environment();
ProcessResult run_process(const ProcessInput& input, const StopRequested& stop);

class Capture {
public:
    void append(std::string_view bytes);
    CapturedOutput finish() const;

private:
    static constexpr std::size_t capacity = 32 * 1024;
    std::string head_;
    std::array<char, capacity> tail_{};
    std::size_t cursor_ = 0;
    std::size_t retained_ = 0;
    std::uint64_t total_ = 0;
};

} // namespace lay::platform
