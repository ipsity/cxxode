#pragma once

#include "config/config.hpp"
#include "core/agent.hpp"

#include <string>
#include <vector>

namespace ode {

struct hook_session {
    std::string id;
    fs::path transcript;
    fs::path cwd;
};

struct hook_context {
    hook_type type;
    hook_session session;
    std::string tool_name;    // pre/post tool
    json tool_input;          // pre/post tool
    tool_output output;       // post tool
    std::string user_prompt;  // prompt submit
    bool stop_hook_active = false;
};

// Runs one hook command with the context as JSON on stdin. Exit code 2 blocks.
hook_verdict execute_hook(const hook_config& config, const hook_context& ctx, cancel_token& cancel,
                          const std::optional<fs::path>& bash);

// Adapts the hooks from settings.json to the loop's extension points.
using script_hooks = std::function<hook_verdict(hook_type, const json&, cancel_token&)>;
agent_hooks make_agent_hooks(std::vector<hook_config> hooks, std::function<hook_session()> session,
                             script_hooks scripts = {});
hook_verdict parse_hook_output(hook_type type, const json& output);

} // namespace ode
