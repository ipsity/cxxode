#pragma once

#include "tools/tool.hpp"

#include <string>
#include <vector>

namespace lay {

enum class permission_mode { plan, normal, yolo };
enum class verdict { allow, deny, ask };

std::string_view mode_name(permission_mode mode);
permission_mode next_mode(permission_mode mode);
// Accepts lay names and Claude Code's defaultMode names.
std::optional<permission_mode> parse_mode(std::string_view text);

// Rules use Claude Code syntax: "Read", "Bash", "Bash(git status)", "Bash(npm run:*)".
class permission_rules {
public:
    permission_mode mode = permission_mode::normal;
    std::vector<std::string> allow;
    std::vector<std::string> deny;

    verdict check(const tool& t, const json& input, const fs::path& root) const;
    // Rule a user grants with "always", e.g. Bash(npm run:*).
    static std::string always_rule(const tool& t, const json& input);
};

// Splits a shell command on && || ; | and newlines, outside quotes.
std::vector<std::string> command_segments(std::string_view command);

} // namespace lay
