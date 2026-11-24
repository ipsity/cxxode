#pragma once

#include "tools/tool.hpp"

#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace lay {

struct skill {
    std::string name;
    std::string description;
    std::filesystem::path path;
    std::string body;  // Lazy-loaded on first use

    std::string load_body() const;
};

// Parses YAML frontmatter from markdown. Returns the metadata and the body after the frontmatter.
std::pair<std::map<std::string, std::string>, std::string> parse_frontmatter(std::string_view text);

// Scans ~/.claude/skills/ and workspace/.claude/skills/ for SKILL.md files
std::vector<skill> load_skills(const std::filesystem::path& workspace);

// Formats skills as a system prompt section
std::string skills_system_prompt(const std::vector<skill>& skills);

// Creates the full tool list including builtin tools and the skill tool (if skills are available)
std::vector<tool> make_tools(const std::vector<skill>& skills);

} // namespace lay
