#include "skills/skill.hpp"
#include "agents/subagent.hpp"
#include "mcp/mcp_client.hpp"

namespace ode {

// Forward declaration
std::vector<tool> make_mcp_tools(const std::vector<std::shared_ptr<mcp_client>>&);

std::vector<tool> make_tools(const std::vector<skill>& skills,
                              const std::vector<std::shared_ptr<mcp_client>>& mcp_clients,
                              const std::vector<agent_config>* agents,
                              provider* model_provider,
                              permission_rules* rules,
                              const fs::path* workspace) {
    auto tools = builtin_tools();

    // Add skill tool
    if (!skills.empty()) {
        tool skill_tool;
        skill_tool.name = "skill";
        skill_tool.description = "Invoke a predefined skill by name. Skills are specialized workflows or commands.";
        skill_tool.schema = {
            {"type", "object"},
            {"properties", {
                {"name", {{"type", "string"}, {"description", "Name of the skill to invoke"}}},
                {"instruction", {{"type", "string"}, {"description", "Optional instruction or context for the skill"}}}
            }},
            {"required", {"name"}}
        };
        skill_tool.access = tool_access::execute;

        skill_tool.run = [skills](const json& input, tool_context&) -> tool_output {
            const auto name = input.value("name", std::string{});
            if (name.empty()) return {"Skill name is required.", true, "缺少名称", ""};

            for (const auto& s : skills) {
                if (s.name == name) {
                    const auto body = read_file(s.path);
                    if (!body) return {"Could not load skill " + name, true, "加载失败", ""};
                    auto [meta, content] = parse_frontmatter(*body);
                    return {content, false, "已加载", ""};
                }
            }
            return {"Unknown skill: " + name, true, "未知 skill", ""};
        };

        tools.push_back(std::move(skill_tool));
    }

    // Add MCP tools
    auto mcp_tools = make_mcp_tools(mcp_clients);
    tools.insert(tools.end(), mcp_tools.begin(), mcp_tools.end());

    // Add task tool (subagents)
    if (agents && !agents->empty() && model_provider && rules && workspace) {
        tool task_tool;
        task_tool.name = "task";
        task_tool.description = "Run a subagent in isolated context. Returns final text output.";
        task_tool.schema = {
            {"type", "object"},
            {"properties", {
                {"agent", {{"type", "string"}, {"description", "Agent name"}}},
                {"task", {{"type", "string"}, {"description", "Task description"}}}
            }},
            {"required", {"agent", "task"}}
        };
        task_tool.access = tool_access::execute;

        auto agents_copy = *agents;
        auto provider_ptr = model_provider;
        auto rules_ptr = rules;
        auto workspace_copy = *workspace;
        auto tools_copy = tools;

        task_tool.run = [agents_copy, provider_ptr, rules_ptr, workspace_copy, tools_copy]
                        (const json& input, tool_context&) -> tool_output {
            const auto agent_name = input.value("agent", std::string{});
            const auto task = input.value("task", std::string{});

            if (agent_name.empty() || task.empty()) {
                return {"Agent and task required.", true, "缺少参数", ""};
            }

            for (const auto& config : agents_copy) {
                if (config.name == agent_name) {
                    return run_subagent(config, task, *provider_ptr, tools_copy, *rules_ptr, workspace_copy);
                }
            }

            return {"Unknown agent: " + agent_name, true, "未知代理", ""};
        };

        tools.push_back(std::move(task_tool));
    }

    return tools;
}

} // namespace ode
