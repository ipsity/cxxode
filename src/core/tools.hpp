#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace lay {

using ArgumentValue = std::variant<std::string, std::int64_t, std::uint64_t, bool, double, std::nullptr_t>;

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

enum class ToolStatus { pending, ok, failed, skipped };

struct ToolResult {
    ToolStatus status = ToolStatus::pending;
    std::string text;
    bool operator==(const ToolResult&) const = default;
};

struct Parameter {
    std::string_view name;
    std::string_view description;
    bool required;
    bool integer;
    std::int64_t minimum;
    std::int64_t maximum;
};

struct ToolSpec {
    std::string_view name;
    std::string_view description;
    std::span<const Parameter> parameters;
};

inline constexpr Parameter read_parameters[] = {
    {"path", "Workspace-relative path, using / as separator. Use . for the root directory.",
     true, false, 1, 2048},
    {"start_line", "First 1-based file line or directory entry. Defaults to 1.",
     false, true, 1, 1000000},
    {"line_count", "Maximum lines or directory entries. Defaults to 120.",
     false, true, 1, 400},
};
inline constexpr Parameter search_parameters[] = {
    read_parameters[0],
    {"query", "Literal case-sensitive text on one line.", true, false, 1, 256},
    {"file_suffix", "Optional case-sensitive filename suffix such as .cpp.",
     false, false, 1, 64},
    {"max_results", "Maximum matching lines. Defaults to 30.", false, true, 1, 100},
};
inline constexpr ToolSpec tool_specs[] = {
    {"read", "Read numbered UTF-8 lines or list immediate directory entries.", read_parameters},
    {"search", "Find literal text under a directory and return workspace paths and line numbers.",
     search_parameters},
};

std::string_view text_arg(const ToolCall& call, std::string_view name);
std::size_t number_arg(const ToolCall& call, std::string_view name, std::size_t fallback);
std::string_view validate(const ToolCall& call);

} // namespace lay
