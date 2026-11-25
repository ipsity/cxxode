#pragma once

#include "tools/tool.hpp"

#include <filesystem>
#include <string>
#include <vector>

namespace ode {

struct skill {
    std::string name;
    std::string description;
    std::filesystem::path path;
    std::string body;  // Lazy-loaded on first use

    std::string load_body() const;
};

std::vector<skill> load_skills(const std::filesystem::path& workspace);

// Name and description of every skill; the body is loaded by the skill tool on demand.
std::string skills_system_prompt(const std::vector<skill>& skills);

// The `skill` tool. Not created when there are no skills.
std::vector<tool> make_skill_tools(const std::vector<skill>& skills);

} // namespace ode
