#pragma once

#include "config/config.hpp"
#include "core/agent.hpp"

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

namespace ode {

struct lua_script {
    std::filesystem::path path;
    std::string plugin_name;
    std::string plugin_version;
};

struct lua_key_result {
    bool handled = false;
    std::string error;
};

class lua_host {
public:
    explicit lua_host(std::function<void(std::string_view)> print = {});
    ~lua_host();
    lua_host(const lua_host&) = delete;
    lua_host& operator=(const lua_host&) = delete;

    // Replaced only between turns. Callers report the diagnostics through their frontend.
    std::vector<std::string> reload(std::vector<std::filesystem::path> scripts, cancel_token& cancel);
    std::vector<std::string> reload_scripts(std::vector<lua_script> scripts, cancel_token& cancel);
    bool active() const;
    std::vector<tool> tools();
    const std::vector<std::string>& commands() const;
    bool has_hooks() const;
    hook_verdict run_hooks(hook_type type, const json& input, cancel_token& cancel,
                           hook_verdict (*parse)(hook_type, const json&));
    tool_output run_command(std::string_view name, std::string_view args, tool_context& ctx);
    bool has_key(std::string_view name) const;
    std::vector<std::string> keys() const;
    lua_key_result run_key(std::string_view name, cancel_token& cancel);
    std::shared_ptr<const std::string> status() const;

private:
    struct runtime;
    std::atomic<std::shared_ptr<const std::string>> status_;
    std::function<void(std::string_view)> print_;
    std::unique_ptr<runtime> vm_;
};

} // namespace ode
