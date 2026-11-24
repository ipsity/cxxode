#include "agents/subagent.hpp"
#include "core/session.hpp"
#include "skills/skill.hpp"

namespace ode {

std::vector<agent_config> load_agents(const fs::path& workspace) {
    std::vector<agent_config> agents;
    std::vector<fs::path> search_paths;

    // User-level agents
    const auto home = ode_home().parent_path();
    search_paths.push_back(home / ".claude" / "agents");

    // Workspace agents - walk up to git root
    for (auto dir = workspace; ; dir = dir.parent_path()) {
        search_paths.push_back(dir / ".claude" / "agents");
        if (fs::exists(dir / ".git") || dir == dir.parent_path()) break;
    }

    for (const auto& dir : search_paths) {
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

} // namespace ode
