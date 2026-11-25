#pragma once

#include "ext/agents.hpp"
#include "ext/commands.hpp"
#include "config/config.hpp"
#include "core/agent.hpp"
#include "app/command_history.hpp"
#include "app/error_handler.hpp"
#include "app/goal_manager.hpp"
#include "app/input_validator.hpp"
#include "app/tool_suggestion.hpp"
#include "ext/mcp.hpp"
#include "ext/skills.hpp"
#include "ui/theme.hpp"

#include <memory>
#include <optional>
#include <string>

namespace ode {

class lua_host;

// Everything one running ode process owns.
struct app {
    fs::path root;
    settings config;
    permission_rules permissions;
    provider_config provider_cfg;
    std::optional<std::string> credential_error;
    std::unique_ptr<provider> model_provider;
    std::unique_ptr<agent> core;
    session_log log;
    std::string session_id;
    std::vector<skill> skills;
    std::vector<user_command> commands;
    std::vector<std::shared_ptr<mcp_client>> mcp_clients;
    std::vector<agent_config> agents;
    lua_host* lua = nullptr;
    std::function<std::vector<std::string>(cancel_token&)> reload_lua;

    // Current theme
    theme current_theme = dark_theme();

    // Week 2 Day 8: Tool suggestion engine
    tool_suggestion_engine suggestion_engine;

    // Week 2 Day 9: Error handler
    error_handler error_handler_;

    // Week 2 Day 10: Command history
    command_history history;

    // Week 2 Day 12: Input validator
    input_validator validator;

    // 🆕 Goal 管理系统
    goal_manager goal_mgr;

    // Opens a new session file, or appends to `resume` after loading it.
    bool open_session(const std::optional<fs::path>& resume);
    void set_model(const std::string& requested);
    void refresh_credentials();
};

int run_interactive(app& a, std::string first_prompt);
int run_print(app& a, std::string prompt);

std::string short_count(std::size_t n);
std::string git_branch(const fs::path& root);
std::string tool_label(const tool& t, const json& input, const fs::path& root);
std::string home_relative(const fs::path& path);

} // namespace ode
