#pragma once

#include "base/cancel.hpp"

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

private:
    struct runtime;
    std::unique_ptr<runtime> vm_;
};

} // namespace ode
