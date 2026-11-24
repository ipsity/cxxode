#include "skills/skill.hpp"

namespace lay {

std::vector<tool> make_tools(const std::vector<skill>& skills) {
    auto tools = builtin_tools();

    if (!skills.empty()) {
        // Create the skill tool that can invoke any loaded skill
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

        // Capture skills by value in the lambda
        skill_tool.run = [skills](const json& input, tool_context&) -> tool_output {
            const auto name = input.value("name", std::string{});
            if (name.empty()) return {"Skill name is required.", true, "缺少名称", {}};

            for (const auto& s : skills) {
                if (s.name == name) {
                    const auto body = read_file(s.path);
                    if (!body) return {"Could not load skill " + name, true, "加载失败", {}};
                    auto [meta, content] = parse_frontmatter(*body);
                    // TODO: Variable expansion for $ARGUMENTS, $WORKSPACE, etc.
                    return {content, false, "已加载", {}};
                }
            }
            return {"Unknown skill: " + name, true, "未知 skill", {}};
        };

        tools.push_back(std::move(skill_tool));
    }

    return tools;
}

} // namespace lay
