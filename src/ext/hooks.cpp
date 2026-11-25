#include "ext/hooks.hpp"
#include "platform/process.hpp"

#include <memory>

namespace ode {

hook_verdict execute_hook(const hook_config& config, const hook_context& ctx, cancel_token& cancel) {
    json input = {
        {"type", [&]() {
            switch (ctx.type) {
                case hook_type::pre_tool_use: return "PreToolUse";
                case hook_type::post_tool_use: return "PostToolUse";
                case hook_type::user_prompt_submit: return "UserPromptSubmit";
                case hook_type::stop: return "Stop";
            }
            return "Unknown";
        }()}
    };

    if (ctx.type == hook_type::pre_tool_use || ctx.type == hook_type::post_tool_use) {
        input["tool"] = ctx.tool_name;
        if (ctx.type == hook_type::pre_tool_use) {
            input["input"] = ctx.tool_input;
        } else {
            input["output"] = ctx.output.text;
            input["isError"] = ctx.output.is_error;
        }
    } else if (ctx.type == hook_type::user_prompt_submit) {
        input["prompt"] = ctx.user_prompt;
    } else if (ctx.type == hook_type::stop) {
        input["reason"] = ctx.stop_reason;
    }

    process_request req;
    req.program = config.command;
    req.arguments = config.args;
    req.directory = config.working_dir.empty() ? fs::current_path() : config.working_dir;
    req.timeout = std::chrono::milliseconds(5000);
    req.stdin_data = input.dump();

    const auto result = run_process(req, cancel);

    // A hook that cannot run must not lock the user out of every tool, so it fails open.
    if (!result.exit_code) return {true, result.error.empty() ? "Hook did not complete" : result.error};
    return {*result.exit_code != 2, result.output};
}

agent_hooks make_agent_hooks(std::vector<hook_config> list) {
    if (list.empty()) return {};
    const auto hooks = std::make_shared<const std::vector<hook_config>>(std::move(list));
    const auto first_block = [hooks](const hook_context& ctx, cancel_token& cancel) -> hook_verdict {
        for (const auto& h : *hooks) {
            if (h.type != ctx.type) continue;
            if (auto v = execute_hook(h, ctx, cancel); !v.allow) return v;
        }
        return {};
    };
    const auto run_all = [hooks](const hook_context& ctx, cancel_token& cancel) {
        for (const auto& h : *hooks) {
            if (h.type == ctx.type) execute_hook(h, ctx, cancel);
        }
    };

    agent_hooks out;
    out.prompt = [first_block](std::string_view prompt, cancel_token& cancel) {
        hook_context ctx;
        ctx.type = hook_type::user_prompt_submit;
        ctx.user_prompt = std::string(prompt);
        return first_block(ctx, cancel);
    };
    out.pre_tool = [first_block](const tool& t, const json& input, cancel_token& cancel) {
        hook_context ctx;
        ctx.type = hook_type::pre_tool_use;
        ctx.tool_name = t.name;
        ctx.tool_input = input;
        return first_block(ctx, cancel);
    };
    out.post_tool = [run_all](const tool& t, const json& input, const tool_output& output, cancel_token& cancel) {
        hook_context ctx;
        ctx.type = hook_type::post_tool_use;
        ctx.tool_name = t.name;
        ctx.tool_input = input;
        ctx.output = output;
        run_all(ctx, cancel);
    };
    out.stop = [run_all](std::string_view reason, cancel_token& cancel) {
        hook_context ctx;
        ctx.type = hook_type::stop;
        ctx.stop_reason = std::string(reason);
        run_all(ctx, cancel);
    };
    return out;
}

} // namespace ode
