#pragma once

#include "base/json.hpp"
#include "config/provider_config.hpp"

#include <chrono>
#include <filesystem>
#include <map>
#include <optional>
#include <regex>
#include <string>
#include <vector>

namespace ode {

// Hook types that can be registered in settings.json
enum class hook_type {
    pre_tool_use,      // Before a tool is executed
    post_tool_use,     // After a tool completes
    user_prompt_submit, // When user submits a prompt
    stop               // Before agent stops (normal or error)
};

// Hook configuration from settings.json
struct hook_config {
    hook_type type;
    std::string matcher;
    std::string command;
    std::chrono::seconds timeout{60};
    std::optional<std::regex> matcher_regex;
};

enum class credential_mode { optional, api_key, bearer, none };

struct provider_entry {
    std::string name;
    protocol kind = protocol::anthropic;
    std::string base_url;
    std::string default_model;
    credential_mode auth = credential_mode::optional;
    std::string api_key{};
};

struct settings {
    std::string provider = "anthropic";
    std::map<std::string, std::string> model_overrides;
    std::map<std::string, provider_entry> providers;
    std::filesystem::path user_settings_path;
    // Permission rules as written in settings.json. main.cpp turns these into a
    // permission_rules, which lives in core and must not be known here.
    std::vector<std::string> permission_allow;
    std::vector<std::string> permission_deny;
    std::string permission_mode;  // empty means the built-in default
    std::vector<hook_config> hooks;  // Hooks loaded from settings.json
    json mcp_servers;  // MCP server configurations from settings.json
    std::vector<std::filesystem::path> loaded_files;
    std::vector<std::string> warnings;
};

// Merges ~/.ode, ~/.claude and the workspace's .claude/.ode settings files.
settings load_settings(const std::filesystem::path& workspace);
std::string configured_model(const settings& s, const std::string& provider);

// Resolves a connection using the key loaded from the user's settings.
std::optional<std::string> resolve_provider(const settings& s, const std::string& name, provider_config& out);
bool save_credential(settings& s, const std::string& provider, const std::string& key);
bool delete_credential(settings& s, const std::string& provider);
bool import_windows_credential(settings& s, const std::string& provider);
bool save_provider_selection(const std::string& provider, const std::filesystem::path& settings_path);

// Built-in model aliases shared with Claude Code agent definitions.
std::string resolve_model_alias(const std::string& model);

// CLAUDE.md, AGENTS.md and ODE.md files that apply to the workspace, user level first.
std::vector<std::pair<std::filesystem::path, std::string>> load_memory(const std::filesystem::path& workspace);

} // namespace ode
