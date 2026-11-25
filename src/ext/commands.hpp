#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace ode {

struct user_command {
    std::string name;
    std::string description;
    std::filesystem::path path;
    std::string body;  // Lazy-loaded

    std::string load_body() const;
};

// Scans ~/.claude/commands/ and workspace/.claude/commands/ for *.md files
std::vector<user_command> load_commands(const std::filesystem::path& workspace);

// Expands variables like $ARGUMENTS, $WORKSPACE in the command body
std::string expand_variables(std::string_view body, const std::map<std::string, std::string>& vars);

} // namespace ode
