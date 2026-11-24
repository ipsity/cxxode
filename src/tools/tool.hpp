#pragma once

#include "core/cancel.hpp"
#include "core/message.hpp"

#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace lay {

namespace fs = std::filesystem;

// Original contents of files changed during one user turn, for /undo.
struct turn_backup {
    std::map<fs::path, std::optional<std::string>> originals;  // nullopt: file did not exist
};

struct file_stamp {
    fs::file_time_type mtime;
    std::uintmax_t size = 0;
    bool operator==(const file_stamp&) const = default;
};

struct tool_context {
    fs::path root;
    cancel_token* cancel = nullptr;
    turn_backup* backup = nullptr;
    std::map<fs::path, file_stamp>* seen = nullptr;  // last stamp the model observed
    std::function<void(std::string_view)> output;     // live bash output
};

struct tool_output {
    std::string text;      // sent to the model
    bool is_error = false;
    std::string summary;   // one line for the UI, e.g. "+4 -1" or "12 lines"
    std::string detail;    // optional extra lines for the UI, e.g. a diff
};

enum class tool_access { read, write, execute };

struct tool {
    std::string name;
    std::string description;
    json schema;
    tool_access access;
    std::function<tool_output(const json& input, tool_context& ctx)> run;
    // Short label for the UI and permission prompt, e.g. the path or the command.
    std::function<std::string(const json& input)> label;
};

std::vector<tool> builtin_tools();

// Helpers shared by the tools and tests.
fs::path resolve(const fs::path& root, std::string_view path);
std::string display_path(const fs::path& root, const fs::path& path);
bool inside(const fs::path& root, const fs::path& path);
std::optional<std::string> read_file(const fs::path& path);
bool glob_match(std::string_view pattern, std::string_view path);
std::vector<fs::path> workspace_files(const fs::path& root, const fs::path& under);
std::string clip(std::string text, std::size_t limit);
std::optional<file_stamp> stamp(const fs::path& path);

// Restores every file in the backup. Returns the display paths restored.
std::vector<std::string> restore(const turn_backup& backup, const fs::path& root);

} // namespace lay
