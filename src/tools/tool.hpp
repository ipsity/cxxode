#pragma once

#include "base/cancel.hpp"
#include "base/files.hpp"
#include "base/json.hpp"
#include "base/text.hpp"

#include <chrono>
#include <filesystem>
#include <functional>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace ode {

namespace fs = std::filesystem;

// Original contents of files changed during one user turn, for /undo.
struct turn_backup {
    std::map<fs::path, std::optional<std::string>> originals;  // nullopt: file did not exist
    void remember(const fs::path& path, std::optional<std::string> original);
};

struct file_stamp {
    fs::file_time_type mtime;
    std::uintmax_t size = 0;
    bool operator==(const file_stamp&) const = default;
};

enum class answer { yes, no, always };
struct tool;
struct tool_output;
struct tool_search_result {
    std::string name;
    std::string description;
    json schema;
};

struct tool_invocation {
    std::string name;
    json input;
};

struct tool_context {
    fs::path root;
    cancel_token* cancel = nullptr;
    turn_backup* backup = nullptr;
    std::map<fs::path, file_stamp>* seen = nullptr;  // last stamp the model observed
    std::function<void(std::string_view)> output;     // live bash output
    std::function<answer(const tool&, const json&)> ask{};
    std::span<const std::string> allowed_tools{};
    std::optional<std::chrono::steady_clock::time_point> deadline;
    std::function<tool_output(std::string_view, const json&, std::optional<std::chrono::steady_clock::time_point>)> invoke_tool{};
    std::function<void(std::string_view)> notice{};
    std::function<std::vector<tool_search_result>(std::string_view, std::size_t, bool)> search_tools{};
    std::function<std::optional<tool_search_result>(std::string_view)> describe_tool{};
    std::function<std::vector<tool_output>(const std::vector<tool_invocation>&,
                                          std::optional<std::chrono::steady_clock::time_point>)> invoke_tools{};
};

struct tool_output {
    std::string text;      // sent to the model
    bool is_error = false;
    std::string summary;   // one line for the UI, e.g. "+4 -1" or "12 lines"
    std::string detail;    // optional extra lines for the UI, e.g. a diff
    std::optional<std::chrono::milliseconds> elapsed{};  // time spent executing, if dispatched
};

enum class tool_access { read, write, execute };
enum class tool_exposure { direct, deferred, codemode };

struct tool {
    std::string name;
    std::string description;
    json schema;
    tool_access access;
    std::function<tool_output(const json& input, tool_context& ctx)> run;
    // Short label for the UI and permission prompt, e.g. the path or the command.
    std::function<std::string(const json& input)> label;
    bool parallel_file_write = false;
    tool_exposure exposure = tool_exposure::direct;
    bool delegates_tool_permissions = false;
};

class tool_registry {
public:
    explicit tool_registry(std::vector<tool> tools = {});

    const tool* find(std::string_view name) const;
    const std::vector<tool>& all() const { return tools_; }
    std::vector<tool> model_tools() const;
    std::vector<tool_search_result> search(std::string_view query, std::size_t limit, bool activate);

private:
    std::vector<tool> tools_;
    std::vector<bool> activated_;
};

std::vector<tool> builtin_tools();
tool make_tool_search_tool();
std::string tool_input_error(const tool& t, const json& input);
// Shell the Bash tool uses on this machine: "bash" or "powershell".
std::string shell_name();
std::optional<fs::path> git_bash();

// Helpers shared by the tools and tests.
fs::path resolve(const fs::path& root, std::string_view path);
std::string display_path(const fs::path& root, const fs::path& path);
bool inside(const fs::path& root, const fs::path& path);
struct write_target {
    fs::path path;
    bool exists;
};
std::optional<write_target> parallel_write_target(const fs::path& root, const fs::path& path);
bool independent_write_targets(const write_target& left, const write_target& right);
bool glob_match(std::string_view pattern, std::string_view path);
std::vector<fs::path> workspace_files(const fs::path& root, const fs::path& under);
std::optional<file_stamp> stamp(const fs::path& path);

// Restores every file in the backup. Returns the display paths restored.
struct restore_result {
    std::vector<std::string> restored;
    std::vector<std::string> failed;
};

restore_result restore_report(const turn_backup& backup, const fs::path& root);
std::vector<std::string> restore(const turn_backup& backup, const fs::path& root);

} // namespace ode
