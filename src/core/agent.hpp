#pragma once

#include "config/config.hpp"
#include "core/permission.hpp"
#include "core/session.hpp"
#include "core/tool_scheduler.hpp"  // 新增：智能调度器
#include "provider/provider.hpp"
#include "tools/tool.hpp"

#include <functional>
#include <string>
#include <vector>

namespace ode {

enum class answer { yes, no, always };

// Callbacks run on the agent's worker thread. ask() blocks until the user decides.
struct agent_events {
    std::function<void()> request_started;
    std::function<void(std::string_view)> text;
    std::function<void(std::string_view text, std::string_view signature)> thinking;
    std::function<void(const tool&, const json& input)> tool_started;
    std::function<void(std::string_view)> tool_progress;  // 兼容：简单文本进度
    std::function<void(const tool&, const json& input, const tool_output&)> tool_finished;
    std::function<answer(const tool&, const json& input)> ask;
    std::function<void(const usage&)> usage_updated;
    std::function<std::vector<std::string>()> take_steering;  // messages typed while the agent works

    // Day 6/7: 详细进度回调（percent, step, timing, resources）
    std::function<void(std::string_view tool_name, int percent, std::string_view step,
                      int64_t elapsed_ms, int64_t estimated_ms, size_t bytes_io, size_t memory_kb)> tool_progress_detail;
};

struct turn_result {
    stop_reason stop = stop_reason::end_turn;
    std::string error;
};

class agent {
public:
    agent(provider& model_provider, std::vector<tool> tools, permission_rules& rules, fs::path root,
          std::vector<hook_config> hooks = {});

    std::string model;
    std::string system;
    std::size_t context_window = 200000;
    std::vector<message> messages;
    session_log* log = nullptr;
    usage total;                  // summed over every request
    std::size_t context_tokens = 0;  // size of the latest request and reply

    turn_result run(std::string user_text, agent_events& events, cancel_token& cancel);
    // Replaces history with a model-written summary.
    turn_result compact(agent_events& events, cancel_token& cancel, std::string_view focus = {});
    // Restores files changed by the most recent turn that changed files.
    std::vector<std::string> undo();
    bool can_undo() const { return !backups_.empty(); }
    const std::vector<tool>& tools() const { return tools_; }
    const tool* find_tool(std::string_view name) const;

private:
    model_request request() const;
    void append(message m);
    void run_tools(const message& reply, agent_events& events, cancel_token& cancel, message& results,
                   turn_backup& backup);
    tool_output run_one(const block& call, agent_events& events, cancel_token& cancel, turn_backup& backup,
                        std::map<fs::path, file_stamp>& seen);

    provider& provider_;
    std::vector<tool> tools_;
    permission_rules& rules_;
    fs::path root_;
    std::vector<turn_backup> backups_;
    std::map<fs::path, file_stamp> seen_;
    std::string pending_note_;
    std::vector<hook_config> hooks_;

    // 新增：智能调度器和进度追踪
    std::unique_ptr<SmartToolScheduler> scheduler_;
    std::unique_ptr<ToolProgressTracker> progress_tracker_;
};

std::string build_system_prompt(const fs::path& root, std::string_view shell,
                                const std::vector<std::pair<fs::path, std::string>>& memory);

} // namespace ode
