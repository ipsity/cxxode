#pragma once

#include "core/cancel.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace ode {

struct process_request {
    std::filesystem::path program;
    std::vector<std::string> arguments;
    std::filesystem::path directory;
    std::chrono::milliseconds timeout{120000};
};

struct process_result {
    std::optional<int> exit_code;
    std::string output;  // stdout and stderr interleaved, as a terminal shows them
    bool timed_out = false;
    bool cancelled = false;
    std::string error;   // could not start or observe the process
    std::chrono::milliseconds elapsed{0};
};

// Runs a child with stdin closed. The whole process tree is killed on timeout,
// cancellation, or shortly after the leader exits while descendants hold the pipe.
process_result run_process(const process_request& request, cancel_token& cancel,
                           const std::function<void(std::string_view)>& on_output = {});

std::optional<std::filesystem::path> find_on_path(std::string_view name);

} // namespace ode
