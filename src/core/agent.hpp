#pragma once

#include "core/permission.hpp"
#include "core/session.hpp"
#include "provider/provider.hpp"
#include "tools/tool.hpp"

#include <functional>
#include <mutex>
#include <string>
#include <vector>

namespace ode {

inline constexpr std::size_t default_context_window = 272000;

struct request_timing {
    std::chrono::milliseconds build_elapsed;
    std::chrono::milliseconds elapsed;
    std::optional<std::chrono::milliseconds> first_output;
    bool compact = false;
    std::optional<provider_timing> provider;
};

// Callbacks from parallel tools are serialized. ask() blocks until the user decides.
struct agent_events {
    std::function<void()> request_started;
    std::function<void(const request_timing&)> request_finished;
    std::function<void(std::string_view)> text;
    std::function<void(std::string_view text, std::string_view signature)> thinking;
    std::function<void(std::string_view)> tool_preparing;
    std::function<void(std::string_view, std::size_t)> tool_argument_progress;
    std::function<void(const tool&, const json& input)> tool_started;
    std::function<void(std::string_view)> tool_progress;
    std::function<void(const tool&, const json& input, const tool_output&)> tool_finished;
    std::function<answer(const tool&, const json& input)> ask;
    std::function<void(const usage&)> usage_updated;
    std::function<void()> compacted;
    std::function<void(std::string_view)> notice;
    std::function<std::vector<std::string>()> take_steering;  // messages typed while the agent works
};

struct turn_result {
    stop_reason stop = stop_reason::end_turn;
    std::string error;
};

struct turn_options {
    std::span<const std::string> allowed_tools{};
    turn_backup* shared_backup = nullptr;
};

struct hook_verdict {
    enum class permission { inherit, allow, ask };
    bool allow = true;
    std::string message;  // shown to the model when the action is blocked
    permission tool_permission = permission::inherit;
    std::string additional_context{};
    std::string warning{};
};

// Extension points the loop calls; core does not know which application module provides them.
struct agent_hooks {
    std::function<hook_verdict(std::string_view prompt, cancel_token&)> prompt;
    std::function<hook_verdict(const tool&, const json& input, cancel_token&)> pre_tool;
    std::function<hook_verdict(const tool&, const json& input, const tool_output&, cancel_token&)> post_tool;
    std::function<hook_verdict(bool active, cancel_token&)> stop;
};

class agent {
public:
    agent(provider& model_provider, std::vector<tool> tools, permission_rules& rules, fs::path root,
          agent_hooks hooks = {});

    std::string model;
    std::string system;
    std::size_t context_window = default_context_window;
    double auto_compact_ratio = 0.8;
    std::vector<message> messages;
    session_log* log = nullptr;
    usage total;                  // summed over every request
    std::size_t context_tokens = 0;  // size of the latest request and reply

    turn_result run(std::string user_text, agent_events& events, cancel_token& cancel, turn_options options = {});
    // Replaces history with a model-written summary.
    turn_result compact(agent_events& events, cancel_token& cancel, std::string_view focus = {});
    // Restores files changed by the most recent turn that changed files.
    std::vector<std::string> undo(std::vector<std::string>* failed = nullptr);
    bool can_undo() const { return !backups_.empty(); }
    const std::vector<tool>& tools() const { return tools_.all(); }
    const tool* find_tool(std::string_view name) const;
    void set_extensions(tool_registry tools, agent_hooks hooks);
    tool_output run_local(const std::function<tool_output(tool_context&)>& action,
                          agent_events& events, cancel_token& cancel);

private:
    model_request request() const;
    model_reply send(const model_request& request, stream_events stream, agent_events& events,
                     cancel_token& cancel, std::chrono::steady_clock::time_point build_started, bool compact = false);
    void append(message m);
    void run_tools(const message& reply, agent_events& events, cancel_token& cancel, message& results,
                   turn_backup& backup, std::span<const std::string> allowed_tools, std::vector<std::string>& steering);
    void run_tool_batch(std::span<const block* const> calls, agent_events& events, cancel_token& cancel,
                        message& results, turn_backup& backup, std::span<const std::string> allowed_tools,
                        bool file_writes, std::span<tool_output> outputs = {},
                        std::optional<std::chrono::steady_clock::time_point> deadline = {});
    std::vector<tool_output> invoke_tool_batch(const std::vector<tool_invocation>& calls,
                                               agent_events& events, cancel_token& cancel,
                                               turn_backup& backup,
                                               std::map<fs::path, file_stamp>& seen,
                                               std::span<const std::string> allowed_tools,
                                               std::optional<std::chrono::steady_clock::time_point> deadline);
    tool_output run_one(const block& call, agent_events& events, cancel_token& cancel, turn_backup& backup,
                        std::map<fs::path, file_stamp>& seen, std::span<const std::string> allowed_tools,
                        std::optional<std::chrono::steady_clock::time_point> deadline = {});
    tool_output run_tool(const tool& t, const json& input, agent_events& events, cancel_token& cancel,
                         turn_backup* backup, std::map<fs::path, file_stamp>& seen,
                         std::span<const std::string> allowed_tools,
                         std::optional<std::chrono::steady_clock::time_point> deadline = {});
    tool_context make_context(tool_access access, agent_events& events, cancel_token& cancel,
                              turn_backup* backup, std::map<fs::path, file_stamp>& seen,
                              std::span<const std::string> allowed_tools, bool delegates_tool_permissions = false,
                              std::optional<std::chrono::steady_clock::time_point> deadline = {});

    provider& provider_;
    tool_registry tools_;
    permission_rules& rules_;
    std::mutex permission_mutex_;
    fs::path root_;
    std::vector<turn_backup> backups_;
    std::map<fs::path, file_stamp> seen_;
    std::string pending_note_;
    agent_hooks hooks_;
};

std::string build_system_prompt(const fs::path& root, std::string_view shell,
                                const std::vector<std::pair<fs::path, std::string>>& memory);

} // namespace ode
