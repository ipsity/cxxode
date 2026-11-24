#pragma once

#include "core/permission.hpp"
#include "platform/process.hpp"
#include "provider/provider.hpp"
#include "tools/tool.hpp"

#include <filesystem>
#include <map>
#include <optional>
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
    std::string command;
    std::vector<std::string> args;
    std::filesystem::path working_dir;
};

struct provider_entry {
    std::string name;
    protocol kind = protocol::anthropic;
    std::string base_url;
    std::string api_key_env;     // environment variable holding the key
    std::string auth_token_env;  // Anthropic bearer token variable
    std::string default_model;
    std::string api_key;         // Direct API key (alternative to env var)
};

struct settings {
    std::string provider = "anthropic";
    std::string model;
    std::map<std::string, provider_entry> providers;
    permission_rules permissions;
    std::vector<hook_config> hooks;  // Hooks loaded from settings.json
    json mcp_servers;  // MCP server configurations from settings.json
    std::vector<std::filesystem::path> loaded_files;
    std::vector<std::string> warnings;
};

// Merges ~/.ode, ~/.claude and the workspace's .claude/.ode settings files.
settings load_settings(const std::filesystem::path& workspace);

// Resolves the API key: environment variable first, then Windows Credential Manager.
// Returns an error message on failure.
std::optional<std::string> resolve_provider(const settings& s, const std::string& name, provider_config& out);
bool save_credential(const std::string& provider, const std::string& key);

std::optional<protocol> parse_protocol(std::string_view text);

// Claude Code aliases: opus, sonnet, haiku, fable. ANTHROPIC_DEFAULT_<NAME>_MODEL overrides
// the built-in id, so a gateway mapping set up for Claude Code applies unchanged.
std::string resolve_model_alias(const std::string& model);

// CLAUDE.md, AGENTS.md and ODE.md files that apply to the workspace, user level first.
std::vector<std::pair<std::filesystem::path, std::string>> load_memory(const std::filesystem::path& workspace);

} // namespace ode
