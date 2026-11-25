#pragma once

#include "core/agent.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ode {

// Subagent configuration from .claude/agents/*.md
struct agent_config {
    std::string name;
    std::string description;
    std::string system_prompt;
    std::filesystem::path path;
};

// Load agent configurations from .claude/agents/ directories
std::vector<agent_config> load_agents(const std::filesystem::path& workspace);

// Execute a subagent in isolated context, return final text only
tool_output run_subagent(const agent_config& config, const std::string& task,
                         provider& model_provider, const std::vector<tool>& tools,
                         permission_rules& rules, const fs::path& workspace);

// The `task` tool. Subagents get `tools` (everything registered before this one).
// Not created when there are no agents.
std::vector<tool> make_task_tools(std::vector<agent_config> agents, provider& model_provider,
                                  std::vector<tool> tools, permission_rules& rules, fs::path workspace);

} // namespace ode
