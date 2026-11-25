#pragma once

#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace ode {

// Shared by every ext module that reads Claude Code style directories. The other ext
// modules depend on this one only, never on each other.

struct plugin_info {
    std::string name;
    std::string version;
    std::string description;
    std::string author;
    std::filesystem::path path;
};

// Plugins in ~/.ode/plugins/ and ~/.claude/plugins/ that have .claude-plugin/plugin.json.
std::vector<plugin_info> load_plugins();

// Directories to scan for one kind of extension ("skills", "commands", "agents"):
// user level, then plugins, then the workspace and its parents up to the git root.
// Later entries override earlier ones with the same name.
std::vector<std::filesystem::path> extension_dirs(const std::filesystem::path& workspace, std::string_view kind);

// Simple YAML frontmatter and its body. Block list entries are separated by newlines.
std::pair<std::map<std::string, std::string>, std::string> parse_frontmatter(std::string_view text);
std::vector<std::string> frontmatter_list(std::string_view value, bool split_spaces = false);

} // namespace ode
