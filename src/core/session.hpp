#pragma once

#include "core/message.hpp"

#include <cstdio>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lay {

// Append-only JSON Lines log. A crash loses at most the line being written;
// load() drops an incomplete last line.
class session_log {
public:
    session_log() = default;
    ~session_log();
    session_log(const session_log&) = delete;
    session_log& operator=(const session_log&) = delete;

    bool open(const std::filesystem::path& path);
    void close();
    bool is_open() const { return file_ != nullptr; }
    const std::filesystem::path& path() const { return path_; }
    void append(const json& line);
    void append_message(const message& m);

private:
    std::FILE* file_ = nullptr;
    std::filesystem::path path_;
};

struct session_info {
    std::filesystem::path path;
    std::string id;
    std::string title;  // first user message, shortened
    std::filesystem::file_time_type modified;
};

struct loaded_session {
    std::vector<message> messages;
    std::string model;
};

std::filesystem::path lay_home();
std::filesystem::path sessions_dir(const std::filesystem::path& workspace);
std::string new_session_id();
loaded_session load_session(const std::filesystem::path& path);
std::vector<session_info> list_sessions(const std::filesystem::path& workspace);

// Every tool_use needs a matching tool_result; patch gaps left by a crash or cancel.
void close_dangling_tool_calls(std::vector<message>& messages);

} // namespace lay
