#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#endif

namespace ode {

// Interactive process with bidirectional stdin/stdout communication
// Used for MCP servers and other long-running subprocesses
class interactive_process {
public:
    struct options {
        std::map<std::string, std::string> env;
        std::filesystem::path working_dir;
    };

    interactive_process(const std::string& command, const std::vector<std::string>& args, const options& opts = {});
    ~interactive_process();

    bool is_running() const;
    std::size_t write_stdin(const void* data, std::size_t size);
    std::size_t read_stdout(void* buffer, std::size_t size);
    std::size_t read_stderr(void* buffer, std::size_t size);
    void terminate();

private:
#ifdef _WIN32
    HANDLE stdin_write_ = nullptr;
    HANDLE stdout_read_ = nullptr;
    HANDLE stderr_read_ = nullptr;
    HANDLE process_ = nullptr;
    DWORD pid_ = 0;
#endif
};

} // namespace ode
