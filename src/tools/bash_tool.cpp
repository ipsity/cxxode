#include "tools/tool.hpp"
#include "platform/process.hpp"

#include <algorithm>
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
    auto timeout = std::clamp<std::int64_t>(input.value("timeout", std::int64_t{120000}), 1000, 600000);
    if (ctx.deadline) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            *ctx.deadline - std::chrono::steady_clock::now()).count();
        if (remaining <= 0) return {"CodeMode execution deadline exceeded", true, "timeout", {}};
        timeout = std::min<std::int64_t>(timeout, remaining);
    }
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
        summary = code == 0 ? "✓" : "✗ exit " + std::to_string(code);
        error = code != 0;
    }
    if (text.empty()) text = "(no output)";
    return {text, error, summary, {}};
}

} // namespace

std::vector<tool> edit_tools();
std::vector<tool> search_tools();

std::string shell_name() { return detect_shell().name; }

namespace {

std::string json_type(const json& value) {
    if (value.is_null()) return "null";
    if (value.is_boolean()) return "boolean";
    if (value.is_number_integer() || value.is_number_unsigned()) return "integer";
    if (value.is_number()) return "number";
    if (value.is_string()) return "string";
    if (value.is_array()) return "array";
    return "object";
}

bool matches_type(const json& value, std::string_view type) {
    if (type == "number") return value.is_number();
    if (type == "integer") return value.is_number_integer() || value.is_number_unsigned();
    return json_type(value) == type;
}

std::optional<std::size_t> size_limit(const json& schema, std::string_view key) {
    const auto it = schema.find(key);
    if (it == schema.end() || !it->is_number_integer()) return std::nullopt;
    if (it->is_number_unsigned()) return it->get<std::size_t>();
    const auto value = it->get<std::int64_t>();
    if (value < 0) return std::nullopt;
    return static_cast<std::size_t>(value);
}

std::string validate_schema(const json& schema, const json& value, const std::string& path) {
    if (!schema.is_object()) return {};
    const auto type = schema.find("type");
    if (type != schema.end()) {
        bool matches = false;
        if (type->is_string()) matches = matches_type(value, type->get_ref<const std::string&>());
        else if (type->is_array()) {
            matches = std::ranges::any_of(*type, [&](const json& item) {
                return item.is_string() && matches_type(value, item.get_ref<const std::string&>());
            });
        }
        if (!matches) return path + " must match type " + type->dump() + ", got " + json_type(value) + ".";
    }
    if (const auto allowed = schema.find("enum"); allowed != schema.end() &&
        std::ranges::find(*allowed, value) == allowed->end()) return path + " must be one of " + allowed->dump() + ".";

    if (value.is_object()) {
        if (const auto required = schema.find("required"); required != schema.end() && required->is_array()) {
            for (const auto& field : *required) {
                if (field.is_string() && !value.contains(field.get_ref<const std::string&>())) {
                    return path + " is missing required field '" + field.get<std::string>() + "'.";
                }
            }
        }
        const auto properties = schema.find("properties");
        const auto additional = schema.find("additionalProperties");
        for (auto it = value.begin(); it != value.end(); ++it) {
            if (properties != schema.end() && properties->is_object()) {
                const auto property = properties->find(it.key());
                if (property != properties->end()) {
                    if (auto error = validate_schema(*property, it.value(), path + "." + it.key()); !error.empty()) return error;
                    continue;
                }
            }
            if (additional != schema.end() && additional->is_boolean() && !additional->get<bool>()) {
                return path + " does not allow field '" + it.key() + "'.";
            }
            if (additional != schema.end() && additional->is_object()) {
                if (auto error = validate_schema(*additional, it.value(), path + "." + it.key()); !error.empty()) return error;
            }
        }
        if (const auto minimum = size_limit(schema, "minProperties"); minimum && value.size() < *minimum)
            return path + " has too few properties.";
        if (const auto maximum = size_limit(schema, "maxProperties"); maximum && value.size() > *maximum)
            return path + " has too many properties.";
    }
    if (value.is_array()) {
        if (const auto items = schema.find("items"); items != schema.end() && items->is_object()) {
            for (std::size_t i = 0; i < value.size(); ++i) {
                if (auto error = validate_schema(*items, value[i], path + "[" + std::to_string(i) + "]"); !error.empty()) return error;
            }
        }
        if (const auto minimum = size_limit(schema, "minItems"); minimum && value.size() < *minimum)
            return path + " has too few items.";
        if (const auto maximum = size_limit(schema, "maxItems"); maximum && value.size() > *maximum)
            return path + " has too many items.";
    }
    if (value.is_string()) {
        const auto length = value.get_ref<const std::string&>().size();
        if (const auto minimum = size_limit(schema, "minLength"); minimum && length < *minimum)
            return path + " is shorter than minLength.";
        if (const auto maximum = size_limit(schema, "maxLength"); maximum && length > *maximum)
            return path + " exceeds maxLength.";
    }
    if (value.is_number()) {
        if (const auto minimum = schema.find("minimum"); minimum != schema.end() && minimum->is_number() && value < *minimum)
            return path + " is below minimum.";
        if (const auto maximum = schema.find("maximum"); maximum != schema.end() && maximum->is_number() && value > *maximum)
            return path + " exceeds maximum.";
    }
    return {};
}

} // namespace

std::string tool_input_error(const tool& t, const json& input) {
    if (!input.is_object()) return t.name + ": input must be a JSON object.";
    if (!t.schema.is_object() || t.schema.empty()) return {};
    return validate_schema(t.schema, input, t.name);
}

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
