#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace ode {

// Plugin metadata from .claude-plugin/plugin.json
struct plugin_info {
    std::string name;
    std::string version;
    std::string description;
    std::string author;
    std::filesystem::path path;
};

// Load plugins from ~/.ode/plugins/ and ~/.claude/plugins/
std::vector<plugin_info> load_plugins();

// Get plugin directories to search for skills, commands, agents
std::vector<std::filesystem::path> plugin_search_paths();

} // namespace ode
