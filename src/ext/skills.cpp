#include "ext/skills.hpp"
#include "base/files.hpp"
#include "ext/discovery.hpp"

#include <algorithm>

namespace ode {

std::string skills_system_prompt(const std::vector<skill>& skills) {
    if (skills.empty()) return {};
    std::string prompt = "\n# Available Skills\n\nYou can invoke pre-defined skills with the `skill` tool:\n\n";
    for (const auto& s : skills) {
        prompt += "- **" + s.name + "**";
        if (!s.description.empty()) prompt += ": " + s.description;
        prompt += "\n";
    }
    prompt += "\nTo use a skill, call `skill` with its name. The skill's instructions will be loaded for you to follow.\n";
    return prompt;
}

std::string skill::load_body() const {
    if (!body.empty()) return body;
    return read_file(path).value_or("");
}

std::vector<skill> load_skills(const fs::path& workspace) {
    std::vector<skill> skills;
    for (const auto& dir : extension_dirs(workspace, "skills")) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;
        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            const bool packaged = entry.is_directory(ec);
            const auto path = packaged ? entry.path() / "SKILL.md" : entry.path();
            if (path.extension() != ".md" || !fs::is_regular_file(path, ec)) continue;
            const auto content = read_file(path);
            if (!content) continue;

            auto [meta, body] = parse_frontmatter(*content);
            const auto name = meta.contains("name") ? meta["name"]
                : (packaged ? entry.path().filename().string() : path.stem().string());
            const auto desc = meta.count("description") ? meta["description"] : "";
            const auto same = std::ranges::find(skills, name, &skill::name);
            if (same != skills.end()) {
                same->path = path;
                same->description = desc;
                same->body = std::move(body);
            } else {
                skills.push_back({name, desc, path, body});
            }
        }
    }
    std::ranges::sort(skills, {}, &skill::name);
    return skills;
}

std::vector<tool> make_skill_tools(const std::vector<skill>& skills) {
    if (skills.empty()) return {};
    tool t;
    t.name = "skill";
    t.description = "Invoke a predefined skill by name. Skills are specialized workflows or commands.";
    t.schema = {
        {"type", "object"},
        {"properties", {
            {"name", {{"type", "string"}, {"description", "Name of the skill to invoke"}}},
            {"instruction", {{"type", "string"}, {"description", "Optional instruction or context for the skill"}}}
        }},
        {"required", {"name"}}
    };
    t.access = tool_access::execute;
    t.run = [skills](const json& input, tool_context&) -> tool_output {
        const auto name = input.value("name", std::string{});
        if (name.empty()) return {"Skill name is required.", true, "缺少名称", ""};
        const auto it = std::ranges::find(skills, name, &skill::name);
        if (it == skills.end()) return {"Unknown skill: " + name, true, "未知 skill", ""};
        const auto text = read_file(it->path);
        if (!text) return {"Could not load skill " + name, true, "加载失败", ""};
        return {parse_frontmatter(*text).second, false, "已加载", ""};
    };
    return {std::move(t)};
}

} // namespace ode
