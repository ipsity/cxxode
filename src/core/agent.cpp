#include "core/agent.hpp"

#include <future>
#include <mutex>

namespace ode {
namespace {

constexpr int max_rounds_without_user = 200;

std::string stop_text(stop_reason stop) {
    switch (stop) {
        case stop_reason::max_tokens: return "The reply hit the output token limit.";
        case stop_reason::refusal: return "The model declined to continue.";
        default: return {};
    }
}

} // namespace

agent::agent(provider& model_provider, std::vector<tool> tools, permission_rules& rules, fs::path root,
             agent_hooks hooks)
    : provider_(model_provider), tools_(std::move(tools)), rules_(rules), root_(std::move(root)), hooks_(std::move(hooks)) {}

const tool* agent::find_tool(std::string_view name) const {
    for (const auto& t : tools_) if (t.name == name) return &t;
    return nullptr;
}

model_request agent::request() const {
    model_request r;
    r.model = model;
    r.system = system;
    r.messages = messages;
    for (const auto& t : tools_) r.tools.push_back({t.name, t.description, t.schema});
    return r;
}

void agent::append(message m) {
    if (log) log->append_message(m);
    messages.push_back(std::move(m));
}

turn_result agent::run(std::string user_text, agent_events& events, cancel_token& cancel, turn_options options) {
    std::string hook_context;
    if (hooks_.prompt) {
        auto v = hooks_.prompt(user_text, cancel);
        if (!v.allow) return {stop_reason::error, v.message.empty() ? "Prompt blocked by hook" : v.message};
        hook_context = std::move(v.additional_context);
    }
    bool auto_compact_failed = false;
    const auto compact_if_needed = [&] {
        if (cancel.cancelled()) return false;
        if (auto_compact_failed || auto_compact_ratio <= 0 || messages.size() <= 2 ||
            static_cast<double>(context_tokens) <= static_cast<double>(context_window) * auto_compact_ratio) return true;
        const auto result = compact(events, cancel);
        if (result.stop == stop_reason::cancelled || cancel.cancelled()) return false;
        if (result.stop == stop_reason::end_turn) {
            if (events.compacted) events.compacted();
        } else {
            auto_compact_failed = true;
            if (events.notice) events.notice("自动压缩失败，继续使用原上下文");
        }
        return true;
    };
    // The new user message belongs verbatim in the first request of this turn.
    if (!compact_if_needed()) return {stop_reason::cancelled, "Cancelled."};

    message user{message::role::user, {}};
    if (!pending_note_.empty()) {
        user.blocks.push_back(block::make_text(pending_note_));
        pending_note_.clear();
    }
    user.blocks.push_back(block::make_text(std::move(user_text)));
    if (!hook_context.empty()) user.blocks.push_back(block::make_text(std::move(hook_context)));
    append(std::move(user));
    turn_backup local_backup;
    auto& backup = options.shared_backup ? *options.shared_backup : local_backup;
    const auto keep_backup = [&] {
        if (!local_backup.originals.empty()) backups_.push_back(std::move(local_backup));
    };
    const auto finish = [&](turn_result r) {
        keep_backup();
        return r;
    };

    bool stop_hook_active = false;
    for (int round = 0; round < max_rounds_without_user; ++round) {
        // Between requests, tool results have already been appended for every call in the previous reply.
        if (round != 0 && !compact_if_needed()) return finish({stop_reason::cancelled, "Cancelled."});
        if (events.request_started) events.request_started();
        stream_events stream;
        stream.text = events.text;
        stream.thinking = [&events](std::string_view text, std::string_view sig) {
            if (events.thinking) events.thinking(text, sig);
        };
        auto reply = provider_.send(request(), stream, cancel);
        if (reply.tokens.input || reply.tokens.output) {
            total.input += reply.tokens.input;
            total.output += reply.tokens.output;
            total.cache_read += reply.tokens.cache_read;
            total.cache_write += reply.tokens.cache_write;
            context_tokens = reply.tokens.context();
            if (events.usage_updated) events.usage_updated(reply.tokens);
        }
        if (!reply.complete) {
            // Keep what the user already saw, but never the tool calls of an incomplete reply.
            const auto shown = reply.content.plain_text();
            if (!shown.empty()) append(message{message::role::assistant, {block::make_text(shown + "\n[reply interrupted]")}});
            keep_backup();
            return {cancel.cancelled() ? stop_reason::cancelled : stop_reason::error, reply.error};
        }
        append(reply.content);

        std::vector<std::string> steering = events.take_steering ? events.take_steering() : std::vector<std::string>{};
        if (reply.stop != stop_reason::tool_use) {
            if (cancel.cancelled()) return finish({stop_reason::cancelled, "Cancelled."});
            if (steering.empty() && hooks_.stop && reply.stop == stop_reason::end_turn) {
                auto v = hooks_.stop(stop_hook_active, cancel);
                if (cancel.cancelled()) return finish({stop_reason::cancelled, "Cancelled."});
                if (!v.allow) {
                    stop_hook_active = true;
                    steering.push_back(v.message.empty() ? "Continue working as requested by the Stop hook." : std::move(v.message));
                }
            }
            if (steering.empty()) return finish({reply.stop, stop_text(reply.stop)});
            message next{message::role::user, {}};
            for (auto& s : steering) next.blocks.push_back(block::make_text(std::move(s)));
            append(std::move(next));
            continue;
        }

        message results{message::role::user, {}};
        run_tools(reply.content, events, cancel, results, backup, options.allowed_tools);
        if (events.take_steering) {
            for (auto& s : events.take_steering()) steering.push_back(std::move(s));
        }
        for (auto& s : steering) {
            results.blocks.push_back(block::make_text("[The user added while you were working]\n" + s));
        }
        append(std::move(results));
        if (cancel.cancelled()) {
            return finish({stop_reason::cancelled, "Cancelled."});
        }
    }
    return finish({stop_reason::error,
                   "Stopped after " + std::to_string(max_rounds_without_user) + " rounds without user input."});
}

void agent::run_tools(const message& reply, agent_events& events, cancel_token& cancel, message& results,
                      turn_backup& backup, std::span<const std::string> allowed_tools) {
    const auto calls = reply.tool_uses();

    // TODO: 未来可以启用智能调度器
    // 当前保持原有逻辑以确保稳定性
    // 可通过配置开关启用：USE_SMART_SCHEDULER

    // if (use_smart_scheduler) {
    //     run_tools_smart(reply, events, cancel, results, backup);
    //     return;
    // }

    // 现有逻辑保持不变
    std::size_t i = 0;
    while (i < calls.size()) {
        if (cancel.cancelled()) {
            results.blocks.push_back(block::make_tool_result(calls[i]->id, "[Interrupted by the user]", true));
            ++i;
            continue;
        }
        // Consecutive read-only calls run in parallel; anything else runs alone, in order.
        std::size_t end = i + 1;
        const auto* first = find_tool(calls[i]->name);
        if (first && first->access == tool_access::read) {
            while (end < calls.size()) {
                const auto* t = find_tool(calls[end]->name);
                if (!t || t->access != tool_access::read) break;
                ++end;
            }
        }
        if (end - i == 1) {
            auto out = run_one(*calls[i], events, cancel, backup, seen_, allowed_tools);
            results.blocks.push_back(block::make_tool_result(calls[i]->id, std::move(out.text), out.is_error));
        } else {
            // Each parallel read records file stamps privately; they are merged afterwards.
            // Callbacks are serialised so event consumers never see concurrent calls.
            std::mutex callback_mutex;
            agent_events serial = events;
            if (events.tool_started) serial.tool_started = [&](const tool& t, const json& in) {
                std::lock_guard lock(callback_mutex);
                events.tool_started(t, in);
            };
            if (events.tool_finished) serial.tool_finished = [&](const tool& t, const json& in, const tool_output& o) {
                std::lock_guard lock(callback_mutex);
                events.tool_finished(t, in, o);
            };
            if (events.tool_progress) serial.tool_progress = [&](std::string_view s) {
                std::lock_guard lock(callback_mutex);
                events.tool_progress(s);
            };
            if (events.ask) serial.ask = [&](const tool& t, const json& in) {
                std::lock_guard lock(callback_mutex);
                return events.ask(t, in);
            };
            std::vector<std::map<fs::path, file_stamp>> stamps(end - i);
            std::vector<std::future<tool_output>> pending;
            for (std::size_t k = i; k < end; ++k) {
                pending.push_back(std::async(std::launch::async, [&, k] {
                    return run_one(*calls[k], serial, cancel, backup, stamps[k - i], allowed_tools);
                }));
            }
            for (std::size_t k = i; k < end; ++k) {
                auto out = pending[k - i].get();
                results.blocks.push_back(block::make_tool_result(calls[k]->id, std::move(out.text), out.is_error));
                for (auto& [path, s] : stamps[k - i]) seen_[path] = s;
            }
        }
        i = end;
    }
}

tool_output agent::run_one(const block& call, agent_events& events, cancel_token& cancel, turn_backup& backup,
                           std::map<fs::path, file_stamp>& seen, std::span<const std::string> allowed_tools) {
    const auto* t = find_tool(call.name);
    if (!t) return {"Unknown tool: " + call.name, true, "未知工具", {}};
    if (!call.input.is_object()) return {"Tool input must be a JSON object.", true, "参数错误", {}};
    tool_output out;
    const auto blocked = [&](std::string reason) {
        tool_output denied{std::move(reason), true, "已拒绝", {}};
        if (events.tool_started) events.tool_started(*t, call.input);
        if (events.tool_finished) events.tool_finished(*t, call.input, denied);
        return denied;
    };
    auto hook = hooks_.pre_tool ? hooks_.pre_tool(*t, call.input, cancel) : hook_verdict{};
    if (cancel.cancelled()) return blocked("[Interrupted by the user]");
    if (!hook.allow) return blocked(hook.message.empty() ? "Blocked by hook" : std::move(hook.message));

    auto permission = [&] {
        std::lock_guard lock(permission_mutex_);
        return rules_.check(*t, call.input, root_, allowed_tools);
    }();
    if (permission != verdict::deny) {
        if (hook.tool_permission == hook_verdict::permission::ask) permission = verdict::ask;
        if (hook.tool_permission == hook_verdict::permission::allow) permission = verdict::allow;
    }
    switch (permission) {
        case verdict::deny:
            return blocked(rules_.mode() == permission_mode::plan
                ? "Plan mode is read-only. Describe the change instead; the user can switch modes with Shift+Tab."
                : "Denied by a permission rule.");
        case verdict::ask: {
            const auto a = events.ask ? events.ask(*t, call.input) : answer::no;
            if (a == answer::no) {
                return blocked("The user declined this tool call. Ask what they want instead, or try another approach.");
            }
            if (a == answer::always) {
                std::lock_guard lock(permission_mutex_);
                rules_.allow.push_back(permission_rules::always_rule(*t, call.input));
            }
            break;
        }
        case verdict::allow: break;
    }

    if (events.tool_started) events.tool_started(*t, call.input);

    tool_context ctx{root_, &cancel, t->access == tool_access::read ? nullptr : &backup, &seen, events.tool_progress,
                     events.ask, allowed_tools};
    try {
        out = t->run(call.input, ctx);
    } catch (const std::exception& e) {
        out = {std::string("Tool failed: ") + e.what(), true, "失败", {}};
    }

    if (events.tool_finished) events.tool_finished(*t, call.input, out);

    if (hooks_.post_tool && !cancel.cancelled()) {
        const auto post = hooks_.post_tool(*t, call.input, out, cancel);
        if (!post.allow && !post.message.empty()) out.text += "\n[PostToolUse hook] " + post.message;
        if (!post.additional_context.empty()) out.text += "\n" + post.additional_context;
    }
    if (!hook.additional_context.empty()) out.text += "\n" + hook.additional_context;

    return out;
}

std::vector<std::string> agent::undo() {
    if (backups_.empty()) return {};
    auto restored = restore(backups_.back(), root_);
    backups_.pop_back();
    seen_.clear();
    pending_note_ = "[The user undid your file changes from an earlier turn. Restored: ";
    for (std::size_t i = 0; i < restored.size(); ++i) pending_note_ += (i ? ", " : "") + restored[i];
    pending_note_ += ". Re-read files before editing them.]";
    return restored;
}

turn_result agent::compact(agent_events& events, cancel_token& cancel, std::string_view focus) {
    if (messages.empty()) return {stop_reason::end_turn, {}};
    auto r = request();
    r.tools.clear();
    std::string ask = "Summarise this conversation so that work can continue without it. Keep: the user's goals and "
                      "decisions, files read or changed and why, commands run and their outcomes, open problems and "
                      "next steps. Be specific with paths and names. Write plain text, no preamble.";
    if (!focus.empty()) ask += " Focus on: " + std::string(focus);
    r.messages.push_back(message{message::role::user, {block::make_text(ask)}});
    close_dangling_tool_calls(r.messages);
    stream_events quiet;
    auto reply = provider_.send(r, quiet, cancel);
    if (cancel.cancelled() || reply.stop == stop_reason::cancelled) return {stop_reason::cancelled, "Cancelled."};
    if (!reply.complete) return {stop_reason::error, reply.error};
    const auto summary_text = reply.content.plain_text();
    if (reply.stop != stop_reason::end_turn || summary_text.find_first_not_of(" \t\r\n") == std::string::npos) {
        return {stop_reason::error, "The model did not return a complete summary."};
    }
    message summary{message::role::user,
                    {block::make_text("[Summary of the earlier conversation]\n" + summary_text)}};
    if (log) log->append({{"v", 1}, {"type", "compact"}, {"message", to_json(summary)}});
    messages = {summary, message{message::role::assistant, {block::make_text("Understood. I'll continue from the summary.")}}};
    if (log) log->append_message(messages.back());
    context_tokens = reply.tokens.output;
    (void)events;
    return {stop_reason::end_turn, {}};
}

} // namespace ode
