#include "ext/agents.hpp"
#include "base/files.hpp"
#include "ext/discovery.hpp"

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
            const auto name = meta.count("name") ? meta["name"] : path.stem().string();
            const auto desc = meta.count("description") ? meta["description"] : "";

            // Avoid duplicates - workspace agents override user agents
            bool found = false;
            for (auto& a : agents) {
                if (a.name == name) {
                    a.path = path;
                    a.description = desc;
                    a.system_prompt = body;
                    found = true;
                    break;
                }
            }
            if (!found) agents.push_back({name, desc, body, path});
        }
    }

    std::ranges::sort(agents, {}, &agent_config::name);
    return agents;
}

tool_output run_subagent(const agent_config& config, const std::string& task,
                         provider& model_provider, const std::vector<tool>& tools,
                         permission_rules& rules, const fs::path& workspace) {
    // Create isolated agent instance
    agent sub_agent(model_provider, tools, rules, workspace);
    sub_agent.system = config.system_prompt;

    // Run the agent with the task
    std::string output;
    bool had_error = false;

    agent_events events;
    events.text = [&output](std::string_view text) { output += text; };
    events.thinking = [](std::string_view, std::string_view) {}; // Ignore thinking
    events.tool_finished = [&had_error](const tool&, const json&, const tool_output& result) {
        if (result.is_error) had_error = true;
    };
    events.ask = [](const tool&, const json&) { return answer::yes; }; // Auto-approve

    cancel_token cancel;
    const auto result = sub_agent.run(task, events, cancel);

    if (result.stop == stop_reason::error || had_error) {
        return {output.empty() ? result.error : output, true, "失败", ""};
    }

    return {output, false, "完成", ""};
}

std::vector<tool> make_task_tools(std::vector<agent_config> agents, provider& model_provider,
                                  std::vector<tool> tools, permission_rules& rules, fs::path workspace) {
    if (agents.empty()) return {};
    tool t;
    t.name = "task";
    t.description = "Run a subagent in isolated context. Returns final text output.";
    t.schema = {
        {"type", "object"},
        {"properties", {
            {"agent", {{"type", "string"}, {"description", "Agent name"}}},
            {"task", {{"type", "string"}, {"description", "Task description"}}}
        }},
        {"required", {"agent", "task"}}
    };
    t.access = tool_access::execute;
    t.run = [agents = std::move(agents), &model_provider, tools = std::move(tools), &rules,
             workspace = std::move(workspace)](const json& input, tool_context&) -> tool_output {
        const auto name = input.value("agent", std::string{});
        const auto task = input.value("task", std::string{});
        if (name.empty() || task.empty()) return {"Agent and task required.", true, "缺少参数", ""};
        for (const auto& config : agents) {
            if (config.name == name) return run_subagent(config, task, model_provider, tools, rules, workspace);
        }
        return {"Unknown agent: " + name, true, "未知代理", ""};
    };
    return {std::move(t)};
}

} // namespace ode
