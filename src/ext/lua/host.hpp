#pragma once

#include "config/config.hpp"
#include "core/agent.hpp"

#include <filesystem>
#include <memory>
#include <vector>
#include <string>

namespace ode {

class lua_host {
public:
    lua_host();
    ~lua_host();
    lua_host(const lua_host&) = delete;
    lua_host& operator=(const lua_host&) = delete;

    // Replaced only between turns. Callers report the diagnostics through their frontend.
    std::vector<std::string> reload(std::vector<std::filesystem::path> scripts, cancel_token& cancel);
    bool active() const;
    std::vector<tool> tools();
    const std::vector<std::string>& commands() const;
    bool has_hooks() const;
    hook_verdict run_hooks(hook_type type, const json& input, cancel_token& cancel,
                           hook_verdict (*parse)(hook_type, const json&));
    tool_output run_command(std::string_view name, std::string_view args, tool_context& ctx);

private:
    struct runtime;
    std::unique_ptr<runtime> vm_;
};

} // namespace ode
