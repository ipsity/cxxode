#include "core/permission.hpp"

#include <algorithm>

namespace ode {
namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

// Commands that only read state; running them never needs a prompt.
constexpr std::string_view read_only_commands[] = {
    "ls", "pwd", "cat", "head", "tail", "wc", "echo", "which", "where", "type", "file", "stat", "du", "df",
    "tree", "rg", "grep", "date", "whoami", "uname", "env", "printenv",
    "git status", "git diff", "git log", "git show", "git branch", "git rev-parse", "git blame", "git ls-files",
    "git remote -v", "git stash list", "Get-ChildItem", "Get-Content", "Get-Location"};

bool has_redirect(std::string_view segment) {
    bool single = false, dbl = false;
    for (const char c : segment) {
        if (c == '\'' && !dbl) single = !single;
        else if (c == '"' && !single) dbl = !dbl;
        else if (!single && !dbl && (c == '>' || c == '`')) return true;
    }
    return segment.find("$(") != std::string_view::npos;
}

bool starts_with_words(std::string_view command, std::string_view prefix) {
    if (!command.starts_with(prefix)) return false;
    return command.size() == prefix.size() || command[prefix.size()] == ' ';
}

// Matches one rule against a tool call. For Bash every command segment must match.
bool rule_matches(std::string_view rule, const tool& t, const json& input) {
    const auto open = rule.find('(');
    const auto name = open == std::string_view::npos ? rule : rule.substr(0, open);
    if (name != t.name) return false;
    if (open == std::string_view::npos || rule.back() != ')') return true;
    const auto pattern = rule.substr(open + 1, rule.size() - open - 2);
    if (t.name != "Bash") {
        const auto path = input.value("file_path", input.value("path", std::string{}));
        return glob_match(pattern, path);
    }
    const auto command = input.value("command", std::string{});
    const bool prefix = pattern.ends_with(":*");
    const auto body = prefix ? pattern.substr(0, pattern.size() - 2) : pattern;
    const auto segments = command_segments(command);
    if (segments.empty()) return false;
    return std::ranges::all_of(segments, [&](const std::string& s) {
        return prefix ? starts_with_words(s, body) : s == body;
    });
}

} // namespace

std::string_view mode_name(permission_mode mode) {
    switch (mode) {
        case permission_mode::plan: return "plan";
        case permission_mode::normal: return "default";
        case permission_mode::yolo: return "yolo";
    }
    return "default";
}

permission_mode next_mode(permission_mode mode) {
    switch (mode) {
        case permission_mode::plan: return permission_mode::normal;
        case permission_mode::normal: return permission_mode::yolo;
        case permission_mode::yolo: return permission_mode::plan;
    }
    return permission_mode::normal;
}

std::optional<permission_mode> parse_mode(std::string_view text) {
    if (text == "plan") return permission_mode::plan;
    if (text == "default" || text == "acceptEdits") return permission_mode::normal;
    if (text == "yolo" || text == "bypassPermissions") return permission_mode::yolo;
    return std::nullopt;
}

std::vector<std::string> command_segments(std::string_view command) {
    std::vector<std::string> out;
    std::string current;
    bool single = false, dbl = false;
    const auto push = [&] {
        const auto t = trim(current);
        if (!t.empty()) out.emplace_back(t);
        current.clear();
    };
    for (std::size_t i = 0; i < command.size(); ++i) {
        const char c = command[i];
        if (c == '\'' && !dbl) single = !single;
        else if (c == '"' && !single) dbl = !dbl;
        if (!single && !dbl && (c == ';' || c == '|' || c == '&' || c == '\n')) {
            push();
            if (i + 1 < command.size() && (command[i + 1] == '|' || command[i + 1] == '&') && command[i + 1] == c) ++i;
            continue;
        }
        current += c;
    }
    push();
    return out;
}

verdict permission_rules::check(const tool& t, const json& input, const fs::path& root) const {
    for (const auto& r : deny) {
        if (rule_matches(r, t, input)) return verdict::deny;
    }
    if (t.access == tool_access::read) return verdict::allow;
    const auto mode = this->mode();
    if (mode == permission_mode::plan) return verdict::deny;
    if (mode == permission_mode::yolo) return verdict::allow;
    for (const auto& r : allow) {
        if (rule_matches(r, t, input)) return verdict::allow;
    }
    if (t.access == tool_access::write) {
        const auto path = resolve(root, input.value("file_path", std::string{}));
        return inside(root, path) ? verdict::allow : verdict::ask;
    }
    if (t.name == "Bash") {
        const auto segments = command_segments(input.value("command", std::string{}));
        const bool safe = !segments.empty() && std::ranges::all_of(segments, [](const std::string& s) {
            if (has_redirect(s)) return false;
            return std::ranges::any_of(read_only_commands, [&](std::string_view c) { return starts_with_words(s, c); });
        });
        if (safe) return verdict::allow;
    }
    return verdict::ask;
}

std::string permission_rules::always_rule(const tool& t, const json& input) {
    if (t.name != "Bash") return t.name;
    // Grant by the leading word, or two words for tools whose subcommand matters.
    static constexpr std::string_view two_word[] = {"git", "npm", "pnpm", "yarn", "cargo", "go", "python", "py",
                                                    "uv", "pip", "dotnet", "docker", "kubectl", "make", "cmake"};
    const auto segments = command_segments(input.value("command", std::string{}));
    if (segments.empty()) return "Bash";
    const std::string_view first = segments.front();
    const auto space = first.find(' ');
    auto prefix = first.substr(0, space);
    if (space != std::string_view::npos && std::ranges::find(two_word, prefix) != std::end(two_word)) {
        const auto next = first.find(' ', space + 1);
        prefix = first.substr(0, next);
    }
    return "Bash(" + std::string(prefix) + ":*)";
}

} // namespace ode
