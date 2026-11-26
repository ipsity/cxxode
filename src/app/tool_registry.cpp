#include "app/tool_registry.hpp"

#include "tools/tool.hpp"

#include <algorithm>
#include <iterator>

namespace ode {

tool_setup build_tool_registry(const std::vector<skill>& skills,
                              const std::vector<std::shared_ptr<mcp_client>>& mcp_clients,
                              std::vector<tool> lua_tools, tool codemode_tool,
                              const std::vector<agent_config>& agents,
                              provider& model_provider, permission_rules& permissions,
                              std::function<std::string()> parent_model,
                              std::vector<hook_config> configured_hooks,
                              std::function<hook_session()> session,
                              script_hooks scripts,
                              std::vector<std::string>& warnings) {
    auto tools = builtin_tools();
    const auto append = [&tools, &warnings](std::vector<tool> additions) {
        for (auto& addition : additions) {
            const auto duplicate = std::ranges::find(tools, addition.name, &tool::name);
            if (duplicate != tools.end()) {
                warnings.push_back("Tool name is already registered: " + addition.name);
                continue;
            }
            tools.push_back(std::move(addition));
        }
    };
    append(make_skill_tools(skills));
    append(make_mcp_tools(mcp_clients));
    std::erase_if(lua_tools, [&](const tool& candidate) {
        const bool conflict = candidate.name == "Task" || candidate.name == "tool_search" || candidate.name == "codemode" ||
            std::ranges::find(tools, candidate.name, &tool::name) != tools.end();
        if (conflict) warnings.push_back("Lua tool name is reserved: " + candidate.name);
        return conflict;
    });
    append(std::move(lua_tools));
    append({std::move(codemode_tool)});
    if (std::ranges::any_of(tools, [](const tool& candidate) {
            return candidate.exposure == tool_exposure::deferred;
        })) {
        tools.push_back(make_tool_search_tool());
    }

    auto hooks = make_agent_hooks(std::move(configured_hooks), std::move(session), std::move(scripts));
    append(make_task_tools(agents, model_provider, tools, permissions, std::move(parent_model), hooks));
    return {tool_registry(std::move(tools)), std::move(hooks)};
}

} // namespace ode
