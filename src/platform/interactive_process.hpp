#pragma once

#include "platform/process.hpp"
#include "platform/win_util.hpp"

#include <chrono>
#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace ode {

namespace fs = std::filesystem;

// A long-lived child with pipes in both directions, for MCP servers.
class interactive_process {
public:
    struct options {
        std::map<std::string, std::string> env;  // overrides, merged over the inherited environment
        fs::path working_dir;
    };

    interactive_process(const std::string& command, const std::vector<std::string>& args, const options& opts = {});
    ~interactive_process();

    interactive_process(const interactive_process&) = delete;
    interactive_process& operator=(const interactive_process&) = delete;

    bool is_running() const;
    // write returns 0 when the pipe is closed or the write fails. read returns 0 on timeout,
    // a closed pipe, or a failed read; check is_running() to tell those apart.
    // read_stderr is only usable before the constructor's drain thread takes the pipe,
    // which it always does today.
    std::size_t write_stdin(const void* data, std::size_t size);
    std::size_t read_stdout(void* buffer, std::size_t size, std::chrono::milliseconds timeout);
    std::size_t read_stderr(void* buffer, std::size_t size);
    // Lets the child see end of input without being killed.
    void close_stdin();
    // Kills the child and everything it spawned.
    void terminate();

private:
#ifdef _WIN32
    HANDLE stdin_write_ = nullptr;
    HANDLE stdout_read_ = nullptr;
    HANDLE stderr_read_ = nullptr;
    win::handle process_;
    win::handle job_;
    DWORD pid_ = 0;
#endif
};

} // namespace ode
