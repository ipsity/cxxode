#include "ext/agents.hpp"
#include "base/files.hpp"
#include "config/config.hpp"
#include "ext/discovery.hpp"

#include <algorithm>

namespace ode {

std::vector<agent_config> load_agents(const fs::path& workspace) {
    std::vector<agent_config> agents;
    for (const auto& dir : extension_dirs(workspace, "agents")) {
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) continue;

        for (const auto& entry : fs::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec)) continue;
            const auto& path = entry.path();
            if (path.extension() != ".md") continue;

            const auto content = read_file(path);
            if (!content) continue;

            auto [meta, body] = parse_frontmatter(*content);
            agent_config config{meta.contains("name") ? meta["name"] : path.stem().string(),
                                meta["description"], std::move(body), path, frontmatter_list(meta["tools"]), meta["model"]};
            const auto found = std::ranges::find(agents, config.name, &agent_config::name);
            if (found == agents.end()) agents.push_back(std::move(config));
            else *found = std::move(config);
        }
    }

    std::ranges::sort(agents, {}, &agent_config::name);
    return agents;
}

namespace {

tool_output run_subagent(const agent_config& config, const std::string& prompt, const std::string& parent_model,
                         provider& model_provider, const std::vector<tool>& tools,
                         permission_rules& rules, const agent_hooks& hooks, tool_context& ctx) {
    std::vector<tool> selected;
    for (const auto& candidate : tools) {
        if (config.tools.empty() || std::ranges::any_of(config.tools, [&](std::string_view selector) {
                if (selector.starts_with("mcp__") && selector.ends_with("__*")) {
                    return candidate.name.starts_with(selector.substr(0, selector.size() - 1));
                }
                return candidate.name == selector;
            })) selected.push_back(candidate);
    }
    if (!config.tools.empty() && selected.empty()) {
        return {"No configured tools are available for agent: " + config.name, true, "工具不可用", {}};
    }
    agent sub_agent(model_provider, std::move(selected), rules, ctx.root, hooks);
    sub_agent.system = config.system_prompt;
    sub_agent.model = config.model.empty() || config.model == "inherit" ? parent_model : resolve_model_alias(config.model);

    agent_events events;
    events.ask = ctx.ask;
    events.notice = ctx.notice;
    cancel_token cancel;
    const auto result = sub_agent.run(prompt, events, ctx.cancel ? *ctx.cancel : cancel, {ctx.allowed_tools, ctx.backup});
    if (result.stop != stop_reason::end_turn) {
        return {result.error.empty() ? "The subagent did not finish." : result.error, true, "失败", {}};
    }
    return {sub_agent.messages.back().plain_text(), false, "完成", {}};
}

} // namespace

std::vector<tool> make_task_tools(std::vector<agent_config> agents, provider& model_provider,
                                  std::vector<tool> tools, permission_rules& rules,
                                  std::function<std::string()> parent_model, const agent_hooks& hooks) {
    if (agents.empty()) return {};
    tool t;
    t.name = "Task";
    t.description = "Run a subagent in isolated context and return its final text. Available agents:";
    for (const auto& config : agents) t.description += "\n- " + config.name + ": " + config.description;
    t.schema = {
        {"type", "object"},
        {"properties", {
            {"subagent_type", {{"type", "string"}, {"description", "Name of the subagent"}}},
            {"description", {{"type", "string"}, {"description", "Short description of the task"}}},
            {"prompt", {{"type", "string"}, {"description", "Instructions to give the subagent"}}}
        }},
        {"required", {"subagent_type", "prompt"}}
    };
    t.access = tool_access::execute;
    agent_hooks child_hooks;
    child_hooks.pre_tool = hooks.pre_tool;
    child_hooks.post_tool = hooks.post_tool;
    t.run = [agents = std::move(agents), &model_provider, tools = std::move(tools), &rules,
             parent_model = std::move(parent_model), child_hooks = std::move(child_hooks)](const json& input, tool_context& ctx) -> tool_output {
        const auto name = input.value("subagent_type", std::string{});
        const auto prompt = input.value("prompt", std::string{});
        if (name.empty() || prompt.empty()) return {"subagent_type and prompt are required.", true, "缺少参数", {}};
        for (const auto& config : agents) {
            if (config.name == name) return run_subagent(config, prompt, parent_model(), model_provider, tools, rules, child_hooks, ctx);
        }
        return {"Unknown agent: " + name, true, "未知代理", {}};
    };
    t.label = [](const json& input) { return input.value("description", input.value("subagent_type", std::string{})); };
    return {std::move(t)};
}

} // namespace ode
