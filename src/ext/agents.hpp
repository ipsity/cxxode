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
    std::vector<std::string> tools;
    std::string model;
};

// Load agent configurations from .claude/agents/ directories
std::vector<agent_config> load_agents(const std::filesystem::path& workspace);

// Task can delegate the tools registered before it.
std::vector<tool> make_task_tools(std::vector<agent_config> agents, provider& model_provider,
                                  std::vector<tool> tools, permission_rules& rules,
                                  std::function<std::string()> parent_model, const agent_hooks& hooks = {});

} // namespace ode
