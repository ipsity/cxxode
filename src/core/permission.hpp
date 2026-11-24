#pragma once

#include "tools/tool.hpp"

#include <atomic>
#include <string>
#include <vector>

namespace ode {

enum class permission_mode { plan, normal, yolo };
enum class verdict { allow, deny, ask };

std::string_view mode_name(permission_mode mode);
permission_mode next_mode(permission_mode mode);
// Accepts ode names and Claude Code's defaultMode names.
std::optional<permission_mode> parse_mode(std::string_view text);

// Rules use Claude Code syntax: "Read", "Bash", "Bash(git status)", "Bash(npm run:*)".
class permission_rules {
public:
    permission_rules() = default;
    permission_rules(const permission_rules& o) : allow(o.allow), deny(o.deny), mode_(o.mode()) {}
    permission_rules& operator=(const permission_rules& o) {
        allow = o.allow;
        deny = o.deny;
        set_mode(o.mode());
        return *this;
    }

    // The UI thread switches modes while the agent thread checks calls.
    permission_mode mode() const { return mode_.load(); }
    void set_mode(permission_mode m) { mode_.store(m); }

    std::vector<std::string> allow;
    std::vector<std::string> deny;

    verdict check(const tool& t, const json& input, const fs::path& root) const;
    // Rule a user grants with "always", e.g. Bash(npm run:*).
    static std::string always_rule(const tool& t, const json& input);

private:
    std::atomic<permission_mode> mode_{permission_mode::normal};
};

// Splits a shell command on && || ; | and newlines, outside quotes.
std::vector<std::string> command_segments(std::string_view command);

} // namespace ode
