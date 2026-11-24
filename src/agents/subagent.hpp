#pragma once

#include "tools/tool.hpp"
#include "core/agent.hpp"

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace lay {

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

} // namespace lay
