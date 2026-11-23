#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace lay {

using TextList = std::vector<std::string>;
using ArgumentValue = std::variant<std::string, std::int64_t, std::uint64_t, bool, double, std::nullptr_t, TextList>;

struct Argument {
    std::string name;
    ArgumentValue value;
    bool operator==(const Argument&) const = default;
};

struct ToolCall {
    std::string id;
    std::string name;
    std::vector<Argument> arguments;
    bool operator==(const ToolCall&) const = default;
};

enum class ToolStatus { pending, ok, failed, skipped, unknown };

struct ToolResult {
    ToolStatus status = ToolStatus::pending;
    std::string text;
    std::string version{};
    std::string observed_utc{};
    bool operator==(const ToolResult&) const = default;
};

struct Parameter {
    enum class Kind { text, integer, text_list };
    std::string_view name;
    std::string_view description;
    bool required;
    Kind kind;
    std::int64_t minimum;
    std::int64_t maximum;
    bool multiline = false;
};

struct ToolSpec {
    std::string_view name;
    std::string_view description;
    std::span<const Parameter> parameters;
};

inline constexpr Parameter read_parameters[] = {
    {"path", "Workspace-relative path, using / as separator. Use . for the root directory.",
     true, Parameter::Kind::text, 1, 2048},
    {"start_line", "First 1-based file line or directory entry. Defaults to 1.",
     false, Parameter::Kind::integer, 1, 1000000},
    {"line_count", "Maximum lines or directory entries. Defaults to 120.",
     false, Parameter::Kind::integer, 1, 400},
};
inline constexpr Parameter search_parameters[] = {
    read_parameters[0],
    {"query", "Literal case-sensitive text on one line.", true, Parameter::Kind::text, 1, 256},
    {"file_suffix", "Optional case-sensitive filename suffix such as .cpp.",
     false, Parameter::Kind::text, 1, 64},
    {"max_results", "Maximum matching lines. Defaults to 30.", false, Parameter::Kind::integer, 1, 100},
};
inline constexpr Parameter edit_parameters[] = {
    read_parameters[0],
    {"version", "The version reported by read, or absent to create a new file.",
     true, Parameter::Kind::text, 1, 64},
    {"old_text", "The exact unique text to replace, with LF newlines. Omit for creation.",
     false, Parameter::Kind::text, 1, 8192, true},
    {"new_text", "Replacement or new file text, with LF newlines.", true, Parameter::Kind::text, 0, 8192, true},
};
inline constexpr Parameter run_parameters[] = {
    {"program", "Executable basename on PATH, absolute path, or workspace-relative path.",
     true, Parameter::Kind::text, 1, 2048},
    {"args", "Individual arguments including empty arguments. The host does not add a shell.",
     true, Parameter::Kind::text_list, 0, 64},
    {"path", "Workspace-relative working directory. Defaults to .",
     false, Parameter::Kind::text, 1, 2048},
    {"timeout_ms", "Process deadline in milliseconds. Defaults to 30000.",
     false, Parameter::Kind::integer, 1, 120000},
};
inline constexpr ToolSpec tool_specs[] = {
    {"read", "Read numbered UTF-8 lines or list immediate directory entries.", read_parameters},
    {"search", "Find literal text under a directory and return workspace paths and line numbers.",
     search_parameters},
    {"edit", "Propose one file replacement for user approval. Read the current version first.",
     edit_parameters},
    {"run", "Propose an executable and arguments for the user's approval, then observe its result.",
     run_parameters},
};

std::string_view text_arg(const ToolCall& call, std::string_view name);
std::size_t number_arg(const ToolCall& call, std::string_view name, std::size_t fallback);
const TextList& list_arg(const ToolCall& call, std::string_view name);
std::string_view validate(const ToolCall& call, bool edit_enabled = false, bool run_enabled = false);

} // namespace lay
