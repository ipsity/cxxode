#include "tools/tool.hpp"
#include "platform/process.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>

namespace ode {
namespace {

struct shell {
    fs::path program;
    std::vector<std::string> prefix;  // arguments before the command
    std::string name;
};

} // namespace

std::optional<fs::path> git_bash() {
    static const auto value = []() -> std::optional<fs::path> {
        std::vector<fs::path> candidates;
        if (auto git = find_on_path("git")) {
            // git.exe lives in <git>/cmd, <git>/bin or <git>/mingw64/bin; bash in <git>/bin.
            for (auto dir = git->parent_path(); dir.has_relative_path(); dir = dir.parent_path()) {
                candidates.push_back(dir / "bin" / "bash.exe");
                if (dir == dir.parent_path()) break;
            }
        }
        for (const char* env : {"ProgramFiles", "ProgramW6432", "LOCALAPPDATA"}) {
            if (const char* dir = std::getenv(env)) {
                candidates.push_back(fs::path(dir) / "Git" / "bin" / "bash.exe");
                candidates.push_back(fs::path(dir) / "Programs" / "Git" / "bin" / "bash.exe");
            }
        }
        for (const auto& c : candidates) {
            std::error_code ec;
            if (fs::is_regular_file(c, ec)) return c;
        }
        return std::nullopt;
    }();
    return value;
}

namespace {

// Git Bash first: models write POSIX commands. PowerShell when Git for Windows is absent.
const shell& detect_shell() {
    static const shell value = [] {
        if (const char* custom = std::getenv("ODE_SHELL"); custom && *custom) {
            return shell{custom, {"-c"}, fs::path(custom).stem().string()};
        }
        if (auto bash = git_bash()) return shell{*bash, {"-c"}, "bash"};
        auto ps = find_on_path("pwsh");
        if (!ps) ps = find_on_path("powershell");
        return shell{ps.value_or("powershell.exe"), {"-NoProfile", "-NonInteractive", "-Command"}, "powershell"};
    }();
    return value;
}

tool_output bash_tool(const json& input, tool_context& ctx) {
    const auto command = input.value("command", std::string{});
    if (command.empty()) return {"command is required.", true, "failed", {}};
    const auto timeout = std::clamp<std::int64_t>(input.value("timeout", std::int64_t{120000}), 1000, 600000);
    const auto& sh = detect_shell();
    process_request req{sh.program, sh.prefix, ctx.root, std::chrono::milliseconds(timeout)};
    req.arguments.push_back(command);
    cancel_token never;
    auto r = run_process(req, ctx.cancel ? *ctx.cancel : never, ctx.output);
    if (!r.error.empty()) return {"Could not start " + sh.name + ": " + r.error, true, "failed to start", {}};

    std::string text = clip(std::move(r.output), 30000);
    std::string summary;
    bool error = false;
    if (r.cancelled) {
        text += "\n[Interrupted by the user]";
        summary = "cancelled";
        error = true;
    } else if (r.timed_out) {
        text += "\n[Killed after " + std::to_string(timeout) + " ms timeout]";
        summary = "timeout";
        error = true;
    } else {
        const int code = r.exit_code.value_or(-1);
        if (code != 0) text += "\n[exit code " + std::to_string(code) + "]";
        char seconds[32];
        std::snprintf(seconds, sizeof(seconds), "%.1fs", static_cast<double>(r.elapsed.count()) / 1000.0);
        summary = code == 0 ? std::string("✓ ") + seconds : "✗ exit " + std::to_string(code) + "  " + seconds;
        error = code != 0;
    }
    if (text.empty()) text = "(no output)";
    return {text, error, summary, {}};
}

} // namespace

std::vector<tool> edit_tools();
std::vector<tool> search_tools();

std::string shell_name() { return detect_shell().name; }

std::vector<tool> builtin_tools() {
    auto tools = edit_tools();
    for (auto& t : search_tools()) tools.push_back(std::move(t));
    tools.push_back({"Bash",
                     "Run a shell command in the workspace directory (" + detect_shell().name +
                         " on this machine). stdin is closed; interactive programs will not work. Output is "
                         "truncated to 30000 characters. Default timeout 120000 ms, maximum 600000.",
                     {{"type", "object"},
                      {"properties",
                       {{"command", {{"type", "string"}}},
                        {"timeout", {{"type", "integer"}, {"description", "Milliseconds"}}},
                        {"description", {{"type", "string"}, {"description", "5-10 word summary of what it does"}}}}},
                      {"required", {"command"}}},
                     tool_access::execute, bash_tool,
                     [](const json& input) { return input.value("command", std::string{}); }});
    return tools;
}

} // namespace ode
