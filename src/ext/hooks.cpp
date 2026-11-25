#include "ext/hooks.hpp"
#include "base/utf.hpp"
#include "platform/process.hpp"

#include <memory>

namespace ode {
namespace {

const char* event_name(hook_type type) {
    switch (type) {
        case hook_type::pre_tool_use: return "PreToolUse";
        case hook_type::post_tool_use: return "PostToolUse";
        case hook_type::user_prompt_submit: return "UserPromptSubmit";
        case hook_type::stop: return "Stop";
    }
    return "";
}

bool matches(const hook_config& config, const hook_context& ctx) {
    if (config.type != ctx.type) return false;
    if (ctx.type != hook_type::pre_tool_use && ctx.type != hook_type::post_tool_use) return true;
    if (config.matcher.empty() || config.matcher == "*") return true;
    return config.matcher_regex ? std::regex_match(ctx.tool_name, *config.matcher_regex) : ctx.tool_name == config.matcher;
}

std::string string_field(const json& object, const char* key) {
    const auto found = object.find(key);
    return found != object.end() && found->is_string() ? found->get<std::string>() : std::string{};
}

void append_context(std::string& context, std::string_view addition) {
    if (addition.empty()) return;
    if (!context.empty()) context += '\n';
    context += addition;
}

json hook_input(const hook_context& ctx) {
    const auto cwd = narrow(ctx.session.cwd.wstring());
    json input{{"session_id", ctx.session.id}, {"transcript_path", narrow(ctx.session.transcript.wstring())},
               {"cwd", cwd}, {"hook_event_name", event_name(ctx.type)}};
    if (ctx.type == hook_type::pre_tool_use || ctx.type == hook_type::post_tool_use) {
        input["tool_name"] = ctx.tool_name;
        input["tool_input"] = ctx.tool_input;
        if (ctx.type == hook_type::post_tool_use) {
            input["tool_response"] = {{"output", ctx.output.text}, {"is_error", ctx.output.is_error}};
        }
    } else if (ctx.type == hook_type::user_prompt_submit) {
        input["prompt"] = ctx.user_prompt;
    } else {
        input["stop_hook_active"] = ctx.stop_hook_active;
    }

    return input;
}

} // namespace

hook_verdict execute_hook(const hook_config& config, const hook_context& ctx, cancel_token& cancel,
                          const std::optional<fs::path>& bash) {
    if (!matches(config, ctx) || cancel.cancelled()) return {};
    process_request req;
    if (bash) {
        req.program = *bash;
        req.arguments = {"-c", config.command};
    } else {
        req.program = find_on_path("cmd.exe").value_or("cmd.exe");
        req.command_tail = "/d /s /c \"" + config.command + '"';
    }
    req.directory = ctx.session.cwd;
    req.timeout = config.timeout;
    req.env = {{"CLAUDE_PROJECT_DIR", narrow(ctx.session.cwd.wstring())}};
    req.separate_stderr = true;
    req.stdin_data = hook_input(ctx).dump() + "\r\n";

    const auto result = run_process(req, cancel);
    if (!result.exit_code || result.cancelled || result.timed_out || !result.error.empty()) return {};
    if (*result.exit_code == 2) return {false, result.stderr_output};
    if (*result.exit_code != 0) return {};

    const auto output = json::parse(result.output, nullptr, false);
    if (output.is_discarded()) {
        hook_verdict verdict;
        if (ctx.type == hook_type::user_prompt_submit) verdict.additional_context = result.output;
        return verdict;
    }
    return parse_hook_output(ctx.type, output);
}

hook_verdict parse_hook_output(hook_type type, const json& output) {
    hook_verdict verdict;
    if (!output.is_object()) return verdict;
    const auto decision = string_field(output, "decision");
    verdict.allow = decision != "block";
    verdict.message = string_field(output, "reason");
    if (type == hook_type::pre_tool_use && decision == "approve") {
        verdict.tool_permission = hook_verdict::permission::allow;
    }
    const auto specific = output.find("hookSpecificOutput");
    if (specific != output.end() && specific->is_object()) {
        if (type == hook_type::pre_tool_use) {
            const auto permission = string_field(*specific, "permissionDecision");
            const auto reason = string_field(*specific, "permissionDecisionReason");
            if (permission == "deny") verdict.allow = false;
            if (permission == "allow") verdict.tool_permission = hook_verdict::permission::allow;
            if (permission == "ask") verdict.tool_permission = hook_verdict::permission::ask;
            if (!permission.empty() && !reason.empty()) verdict.message = reason;
        }
        verdict.additional_context = string_field(*specific, "additionalContext");
    }
    return verdict;
}

agent_hooks make_agent_hooks(std::vector<hook_config> list, std::function<hook_session()> session,
                             script_hooks scripts) {
    if (list.empty() && !scripts) return {};
    const auto hooks = std::make_shared<const std::vector<hook_config>>(std::move(list));
    const auto bash = hooks->empty() ? std::nullopt : git_bash();
    const auto run = [hooks, session = std::move(session), bash, scripts = std::move(scripts)]
        (hook_context ctx, cancel_token& cancel) {
        ctx.session = session();
        hook_verdict merged;
        const auto merge = [&](const hook_verdict& verdict) {
            if (!verdict.allow) {
                merged.allow = false;
                append_context(merged.message, verdict.message);
            }
            if (verdict.tool_permission == hook_verdict::permission::ask ||
                merged.tool_permission == hook_verdict::permission::inherit) {
                merged.tool_permission = verdict.tool_permission;
            }
            append_context(merged.additional_context, verdict.additional_context);
            append_context(merged.warning, verdict.warning);
        };
        for (const auto& hook : *hooks) {
            const auto verdict = execute_hook(hook, ctx, cancel, bash);
            if (!verdict.allow && ctx.type != hook_type::post_tool_use) return verdict;
            merge(verdict);
        }
        if (scripts && !cancel.cancelled()) merge(scripts(ctx.type, hook_input(ctx), cancel));
        return merged;
    };

    agent_hooks out;
    out.prompt = [run](std::string_view prompt, cancel_token& cancel) {
        hook_context ctx{};
        ctx.type = hook_type::user_prompt_submit;
        ctx.user_prompt = prompt;
        return run(std::move(ctx), cancel);
    };
    out.pre_tool = [run](const tool& t, const json& input, cancel_token& cancel) {
        hook_context ctx{};
        ctx.type = hook_type::pre_tool_use;
        ctx.tool_name = t.name;
        ctx.tool_input = input;
        return run(std::move(ctx), cancel);
    };
    out.post_tool = [run](const tool& t, const json& input, const tool_output& output, cancel_token& cancel) {
        hook_context ctx{};
        ctx.type = hook_type::post_tool_use;
        ctx.tool_name = t.name;
        ctx.tool_input = input;
        ctx.output = output;
        return run(std::move(ctx), cancel);
    };
    out.stop = [run](bool active, cancel_token& cancel) {
        hook_context ctx{};
        ctx.type = hook_type::stop;
        ctx.stop_hook_active = active;
        return run(std::move(ctx), cancel);
    };
    return out;
}

} // namespace ode
