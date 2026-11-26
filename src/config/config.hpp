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

struct provider_entry {
    std::string name;
    protocol kind = protocol::anthropic;
    std::string base_url;
    std::string api_key_env;     // environment variable holding the key
    std::string auth_token_env;  // Anthropic bearer token variable
    std::string default_model;
    std::string api_key{};       // Direct API key (alternative to env var)
};

struct settings {
    std::string provider = "anthropic";
    std::map<std::string, std::string> model_overrides;
    std::map<std::string, provider_entry> providers;
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

// Resolves the API key: environment variable first, then Windows Credential Manager.
// Returns an error message on failure.
std::optional<std::string> resolve_provider(const settings& s, const std::string& name, provider_config& out);
bool save_credential(const std::string& provider, const std::string& key);
bool delete_credential(const std::string& provider);
bool save_provider_selection(const std::string& provider, const std::string& previous_provider,
                             const std::filesystem::path& settings_path);

// Claude Code aliases: opus, sonnet, haiku, fable. ANTHROPIC_DEFAULT_<NAME>_MODEL overrides
// the built-in id, so a gateway mapping set up for Claude Code applies unchanged.
std::string resolve_model_alias(const std::string& model);

// CLAUDE.md, AGENTS.md and ODE.md files that apply to the workspace, user level first.
std::vector<std::pair<std::filesystem::path, std::string>> load_memory(const std::filesystem::path& workspace);

} // namespace ode
