#pragma once

#include "platform/process.hpp"
#include "tools/tool.hpp"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace lay {

// Hook types that can be registered in settings.json
enum class hook_type {
    pre_tool_use,      // Before a tool is executed
    post_tool_use,     // After a tool completes
    user_prompt_submit, // When user submits a prompt
    stop               // Before agent stops (normal or error)
};

struct hook_context {
    hook_type type;
    std::string tool_name;    // For pre/post tool hooks
    json tool_input;          // For pre_tool_use
    tool_output tool_output;  // For post_tool_use
    std::string user_prompt;  // For user_prompt_submit
    std::string stop_reason;  // For stop hook
};

struct hook_result {
    bool allow = true;        // If false, blocks the action (exit code 2)
    std::string message;      // Optional message to user
};

// Hook configuration from settings.json
struct hook_config {
    hook_type type;
    std::string command;
    std::vector<std::string> args;
    std::filesystem::path working_dir;
};

// Execute a hook script and return whether to proceed
hook_result execute_hook(const hook_config& config, const hook_context& ctx, cancel_token& cancel);

// Load hooks from settings
std::vector<hook_config> load_hooks(const settings& config);

} // namespace lay
