#include "hooks/hooks.hpp"
#include "config/config.hpp"

namespace ode {

hook_result execute_hook(const hook_config& config, const hook_context& ctx, cancel_token& cancel) {
    // Build input JSON
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
            input["output"] = ctx.tool_output.text;
            input["isError"] = ctx.tool_output.is_error;
        }
    } else if (ctx.type == hook_type::user_prompt_submit) {
        input["prompt"] = ctx.user_prompt;
    } else if (ctx.type == hook_type::stop) {
        input["reason"] = ctx.stop_reason;
    }

    // Run hook script with JSON on stdin
    process_request req;
    req.program = config.command;
    req.arguments = config.args;
    req.directory = config.working_dir.empty() ? fs::current_path() : config.working_dir;
    req.timeout = std::chrono::milliseconds(5000);  // 5s timeout for hooks
    req.stdin_data = input.dump();  // Pass JSON input to hook via stdin

    auto result = run_process(req, cancel);

    hook_result hr;
    if (result.exit_code) {
        // Exit code 2 means block/deny
        hr.allow = (*result.exit_code != 2);
        hr.message = result.output;
    } else {
        // If hook failed to run or was cancelled, allow by default (fail open)
        hr.allow = true;
        hr.message = result.error.empty() ? "Hook did not complete" : result.error;
    }

    return hr;
}

std::vector<hook_config> load_hooks(const settings& s) {
    // Hooks are already parsed from settings.json into s.hooks
    return s.hooks;
}

} // namespace ode
