#pragma once

#include "config/config.hpp"
#include "core/agent.hpp"
#include "ext/agents.hpp"
#include "ext/hooks.hpp"
#include "ext/mcp.hpp"
#include "ext/skills.hpp"

namespace ode {

struct tool_setup {
    tool_registry registry;
    agent_hooks hooks;
};

tool_setup build_tool_registry(const std::vector<skill>& skills,
                              const std::vector<std::shared_ptr<mcp_client>>& mcp_clients,
                              std::vector<tool> lua_tools, const std::vector<agent_config>& agents,
                              provider& model_provider, permission_rules& permissions,
                              std::function<std::string()> parent_model,
                              std::vector<hook_config> configured_hooks,
                              std::function<hook_session()> session,
                              script_hooks scripts,
                              std::vector<std::string>& warnings);

} // namespace ode
