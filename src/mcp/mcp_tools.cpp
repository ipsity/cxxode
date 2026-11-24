#include "mcp/mcp_client.hpp"
#include "skills/skill.hpp"

namespace lay {

// Wraps MCP tools as cxxode tools
std::vector<tool> make_mcp_tools(const std::vector<std::shared_ptr<mcp_client>>& mcp_clients) {
    std::vector<tool> tools;

    for (const auto& client : mcp_clients) {
        if (!client || !client->is_running()) continue;

        const auto mcp_tools = client->list_tools();
        for (const auto& mcp_tool : mcp_tools) {
            tool t;
            // Name format: mcp__servername__toolname
            t.name = "mcp__" + client->server_name() + "__" + mcp_tool.name;
            t.description = mcp_tool.description + " (via MCP server: " + client->server_name() + ")";
            t.schema = mcp_tool.schema;
            t.access = tool_access::execute;  // MCP tools can do anything

            // Capture client and original tool name
            auto client_ptr = client;
            auto original_name = mcp_tool.name;
            t.run = [client_ptr, original_name](const json& input, tool_context&) -> tool_output {
                return client_ptr->call_tool(original_name, input);
            };

            tools.push_back(std::move(t));
        }
    }

    return tools;
}

std::vector<tool> make_tools(const std::vector<skill>& skills,
                              const std::vector<std::shared_ptr<mcp_client>>& mcp_clients) {
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

        skill_tool.run = [skills](const json& input, tool_context& ctx) -> tool_output {
            const auto name = input.value("name", std::string{});
            if (name.empty()) return {"Skill name is required.", true, "缺少名称", {}};

            for (const auto& s : skills) {
                if (s.name == name) {
                    const auto body = read_file(s.path);
                    if (!body) return {"Could not load skill " + name, true, "加载失败", {}};
                    auto [meta, content] = parse_frontmatter(*body);
                    return {content, false, "已加载", {}};
                }
            }
            return {"Unknown skill: " + name, true, "未知 skill", {}};
        };

        tools.push_back(std::move(skill_tool));
    }

    // Add MCP tools
    auto mcp_tools = make_mcp_tools(mcp_clients);
    tools.insert(tools.end(), mcp_tools.begin(), mcp_tools.end());

    return tools;
}

} // namespace lay
