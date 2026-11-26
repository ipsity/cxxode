#include "core/agent.hpp"

#include <future>
#include <mutex>

namespace ode {
namespace {

constexpr int max_rounds_without_user = 200;
constexpr std::size_t max_parallel_tools = 4;

void take_steering(agent_events& events, std::vector<std::string>& steering) {
    if (events.take_steering) {
        for (auto& note : events.take_steering()) steering.push_back(std::move(note));
    }
}

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
    return tools_.find(name);
}

model_request agent::request() const {
    model_request r;
    r.model = model;
    r.system = system;
    r.messages = messages;
    for (const auto& t : tools_.model_tools()) r.tools.push_back({t.name, t.description, t.schema});
    return r;
}

model_reply agent::send(const model_request& request, stream_events stream, agent_events& events,
                        cancel_token& cancel, std::chrono::steady_clock::time_point build_started, bool compact) {
    if (!events.request_finished) return provider_.send(request, stream, cancel);

    using clock = std::chrono::steady_clock;
    using milliseconds = std::chrono::milliseconds;
    const auto started = clock::now();
    const auto build_elapsed = std::chrono::duration_cast<milliseconds>(started - build_started);
    const auto elapsed = [&] { return std::chrono::duration_cast<milliseconds>(clock::now() - started); };
    std::optional<milliseconds> first_output;
    const auto record_first = [&](std::string_view output) {
        if (!output.empty() && !first_output) first_output = elapsed();
    };
    stream_events timed;
    timed.text = [&](std::string_view text) {
        record_first(text);
        if (stream.text) stream.text(text);
    };
    timed.thinking = [&](std::string_view text, std::string_view signature) {
        record_first(text);
        if (stream.thinking) stream.thinking(text, signature);
    };
    timed.tool_started = [&](const std::string& name) {
        record_first(name);
        if (stream.tool_started) stream.tool_started(name);
    };
    auto reply = provider_.send(request, timed, cancel);
    events.request_finished({build_elapsed, elapsed(), first_output, compact, reply.timing});
    return reply;
}

void agent::append(message m) {
    if (log) log->append_message(m);
    messages.push_back(std::move(m));
}

turn_result agent::run(std::string user_text, agent_events& events, cancel_token& cancel, turn_options options) {
    std::string hook_context;
    if (hooks_.prompt) {
        auto v = hooks_.prompt(user_text, cancel);
        if (events.notice && !v.warning.empty()) events.notice(v.warning);
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
        if (events.tool_preparing) stream.tool_started = events.tool_preparing;
        stream.thinking = [&events](std::string_view text, std::string_view sig) {
            if (events.thinking) events.thinking(text, sig);
        };
        const auto build_started = std::chrono::steady_clock::now();
        auto reply = send(request(), std::move(stream), events, cancel, build_started);
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

        std::vector<std::string> steering;
        take_steering(events, steering);
        if (reply.stop != stop_reason::tool_use) {
            if (cancel.cancelled()) return finish({stop_reason::cancelled, "Cancelled."});
            if (steering.empty() && hooks_.stop && reply.stop == stop_reason::end_turn) {
                auto v = hooks_.stop(stop_hook_active, cancel);
                if (events.notice && !v.warning.empty()) events.notice(v.warning);
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
        run_tools(reply.content, events, cancel, results, backup, options.allowed_tools, steering);
        take_steering(events, steering);
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
                      turn_backup& backup, std::span<const std::string> allowed_tools, std::vector<std::string>& steering) {
    const auto calls = reply.tool_uses();
    const auto write_target_for = [&](const tool* t, const block& call) -> std::optional<write_target> {
        if (!t || !t->parallel_file_write || !tool_input_error(*t, call.input).empty()) return std::nullopt;
        {
            std::lock_guard lock(permission_mutex_);
            if (rules_.check(*t, call.input, root_, allowed_tools) != verdict::allow) return std::nullopt;
        }
        return parallel_write_target(root_, resolve(root_, call.input.at("file_path").get_ref<const std::string&>()));
    };

    std::size_t i = 0;
    while (i < calls.size()) {
        if (cancel.cancelled()) {
            results.blocks.push_back(block::make_tool_result(calls[i]->id, "[Interrupted by the user]", true));
            ++i;
            continue;
        }
        std::size_t end = i + 1;
        const auto* first = find_tool(calls[i]->name);
        const bool file_writes = first && first->access == tool_access::write;
        if (first && first->access == tool_access::read) {
            while (end < calls.size() && end - i < max_parallel_tools) {
                const auto* t = find_tool(calls[end]->name);
                if (!t || t->access != tool_access::read) break;
                ++end;
            }
        } else if (file_writes && !hooks_.pre_tool && !hooks_.post_tool) {
            if (auto target = write_target_for(first, *calls[i])) {
                std::vector<write_target> targets{std::move(*target)};
                while (end < calls.size() && end - i < max_parallel_tools) {
                    auto next = write_target_for(find_tool(calls[end]->name), *calls[end]);
                    if (!next || !std::ranges::all_of(targets, [&](const write_target& prior) {
                        return independent_write_targets(prior, *next);
                    })) break;
                    targets.push_back(std::move(*next));
                    ++end;
                }
            }
        }
        if (first && first->access != tool_access::read) {
            take_steering(events, steering);
            if (!steering.empty()) {
                for (; i < calls.size(); ++i) {
                    results.blocks.push_back(block::make_tool_result(calls[i]->id,
                        "Skipped because the user added instructions. Reconsider this call using the new instructions.", true));
                }
                break;
            }
        }
        if (end - i == 1) {
            auto out = run_one(*calls[i], events, cancel, backup, seen_, allowed_tools);
            results.blocks.push_back(block::make_tool_result(calls[i]->id, std::move(out.text), out.is_error));
        } else {
            run_tool_batch(std::span(calls).subspan(i, end - i), events, cancel, results, backup, allowed_tools, file_writes);
        }
        i = end;
    }
}

void agent::run_tool_batch(std::span<const block* const> calls, agent_events& events, cancel_token& cancel,
                           message& results, turn_backup& backup, std::span<const std::string> allowed_tools,
                           bool file_writes, std::span<tool_output> outputs,
                           std::optional<std::chrono::steady_clock::time_point> deadline) {
    // Each call records its own file stamps and backups until its result is collected.
    std::mutex callback_mutex;
    agent_events serial = events;
    const auto serialize = [&callback_mutex](auto& callback) {
        return [&callback_mutex, &callback](auto&&... args) {
            std::lock_guard lock(callback_mutex);
            return callback(std::forward<decltype(args)>(args)...);
        };
    };
    if (events.tool_started) serial.tool_started = serialize(events.tool_started);
    if (events.tool_finished) serial.tool_finished = serialize(events.tool_finished);
    if (events.tool_progress) serial.tool_progress = serialize(events.tool_progress);
    if (events.notice) serial.notice = serialize(events.notice);
    if (events.ask) serial.ask = serialize(events.ask);

    std::vector<std::map<fs::path, file_stamp>> stamps(calls.size());
    std::vector<turn_backup> backups(calls.size());
    if (file_writes) {
        for (std::size_t i = 0; i < calls.size(); ++i) {
            const auto path = resolve(root_, calls[i]->input.at("file_path").get_ref<const std::string&>());
            if (const auto prior = seen_.find(path); prior != seen_.end()) stamps[i].insert(*prior);
        }
    }
    std::vector<std::future<tool_output>> pending;
    pending.reserve(calls.size());
    for (std::size_t i = 0; i < calls.size(); ++i) {
        const auto run = [&, i] {
            if (cancel.cancelled()) return tool_output{"[Interrupted by the user]", true, {}, {}};
            return run_one(*calls[i], serial, cancel, backups[i], stamps[i], allowed_tools, deadline);
        };
        // A deferred call still returns a result if the system cannot start another thread.
        pending.push_back(std::async(std::launch::async | std::launch::deferred, run));
    }
    for (std::size_t i = 0; i < calls.size(); ++i) {
        auto out = pending[i].get();
        if (!outputs.empty()) outputs[i] = out;
        results.blocks.push_back(block::make_tool_result(calls[i]->id, std::move(out.text), out.is_error));
        for (auto& [path, original] : backups[i].originals) backup.remember(path, std::move(original));
        for (const auto& [path, stamp] : stamps[i]) seen_[path] = stamp;
    }
}

std::vector<tool_output> agent::invoke_tool_batch(const std::vector<tool_invocation>& requests,
                                                  agent_events& events, cancel_token& cancel,
                                                  turn_backup& backup, std::map<fs::path, file_stamp>& seen,
                                                  std::span<const std::string> allowed_tools,
                                                  std::optional<std::chrono::steady_clock::time_point> deadline) {
    std::vector<tool_output> outputs(requests.size());
    std::size_t begin = 0;
    while (begin < requests.size()) {
        if (cancel.cancelled()) {
            outputs[begin++] = {"[Interrupted by the user]", true, "已取消", {}};
            continue;
        }
        if (requests[begin].name == "codemode") {
            outputs[begin++] = {"CodeMode scripts cannot invoke codemode recursively.", true, "拒绝递归", {}};
            continue;
        }
        const auto* first = find_tool(requests[begin].name);
        std::size_t end = begin + 1;
        const bool file_writes = first && first->access == tool_access::write;
        const bool mcp_calls = requests[begin].name.starts_with("mcp__");
        if (first && first->access == tool_access::read) {
            while (end < requests.size() && end - begin < max_parallel_tools) {
                const auto* next = find_tool(requests[end].name);
                if (!next || next->access != tool_access::read || requests[end].name == "codemode") break;
                ++end;
            }
        } else if (mcp_calls) {
            while (end < requests.size() && end - begin < max_parallel_tools &&
                   requests[end].name.starts_with("mcp__") && find_tool(requests[end].name)) ++end;
        } else if (file_writes && !hooks_.pre_tool && !hooks_.post_tool) {
            std::vector<write_target> targets;
            const auto target_for = [&](std::size_t index) -> std::optional<write_target> {
                const auto* candidate = find_tool(requests[index].name);
                if (!candidate || !candidate->parallel_file_write ||
                    !tool_input_error(*candidate, requests[index].input).empty()) return std::nullopt;
                {
                    std::lock_guard lock(permission_mutex_);
                    if (rules_.check(*candidate, requests[index].input, root_, allowed_tools) != verdict::allow) return std::nullopt;
                }
                const auto path = requests[index].input.value("file_path", std::string{});
                if (path.empty()) return std::nullopt;
                return parallel_write_target(root_, resolve(root_, path));
            };
            if (auto target = target_for(begin)) {
                targets.push_back(std::move(*target));
                while (end < requests.size() && end - begin < max_parallel_tools) {
                    auto next = target_for(end);
                    if (!next || !std::ranges::all_of(targets, [&](const write_target& prior) {
                            return independent_write_targets(prior, *next);
                        })) break;
                    targets.push_back(std::move(*next));
                    ++end;
                }
            }
        }

        if (end - begin == 1) {
            const auto call = block::make_tool_use(std::to_string(begin), requests[begin].name, requests[begin].input);
            outputs[begin] = run_one(call, events, cancel, backup, seen, allowed_tools, deadline);
        } else {
            std::vector<block> blocks;
            std::vector<const block*> calls;
            blocks.reserve(end - begin);
            calls.reserve(end - begin);
            for (std::size_t i = begin; i < end; ++i) {
                blocks.push_back(block::make_tool_use(std::to_string(i), requests[i].name, requests[i].input));
            }
            for (const auto& call : blocks) calls.push_back(&call);
            message ignored;
            run_tool_batch(calls, events, cancel, ignored, backup, allowed_tools, file_writes,
                           std::span(outputs).subspan(begin, end - begin), deadline);
        }
        begin = end;
    }
    return outputs;
}

tool_output agent::run_one(const block& call, agent_events& events, cancel_token& cancel, turn_backup& backup,
                           std::map<fs::path, file_stamp>& seen, std::span<const std::string> allowed_tools,
                           std::optional<std::chrono::steady_clock::time_point> deadline) {
    const auto* t = find_tool(call.name);
    if (!t) return {"Unknown tool: " + call.name, true, "未知工具", {}};
    return run_tool(*t, call.input, events, cancel, &backup, seen, allowed_tools, deadline);
}

tool_output agent::run_tool(const tool& t, const json& input, agent_events& events, cancel_token& cancel,
                            turn_backup* backup, std::map<fs::path, file_stamp>& seen,
                            std::span<const std::string> allowed_tools,
                            std::optional<std::chrono::steady_clock::time_point> deadline) {
    if (deadline && std::chrono::steady_clock::now() >= *deadline)
        return {"CodeMode execution deadline exceeded", true, "timeout", {}};
    if (auto error = tool_input_error(t, input); !error.empty()) return {std::move(error), true, "参数错误", {}};
    tool_output out;
    const auto blocked = [&](std::string reason) {
        tool_output denied{std::move(reason), true, "已拒绝", {}};
        if (events.tool_started) events.tool_started(t, input);
        if (events.tool_finished) events.tool_finished(t, input, denied);
        return denied;
    };
    auto hook = hooks_.pre_tool ? hooks_.pre_tool(t, input, cancel) : hook_verdict{};
    if (events.notice && !hook.warning.empty()) events.notice(hook.warning);
    if (cancel.cancelled()) return blocked("[Interrupted by the user]");
    if (!hook.allow) return blocked(hook.message.empty() ? "Blocked by hook" : std::move(hook.message));

    auto permission = [&] {
        std::lock_guard lock(permission_mutex_);
        return rules_.check(t, input, root_, allowed_tools);
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
            const auto a = events.ask ? events.ask(t, input) : answer::no;
            if (a == answer::no) {
                return blocked("The user declined this tool call. Ask what they want instead, or try another approach.");
            }
            if (a == answer::always) {
                std::lock_guard lock(permission_mutex_);
                rules_.allow.push_back(permission_rules::always_rule(t, input));
            }
            break;
        }
        case verdict::allow: break;
    }

    if (events.tool_started) events.tool_started(t, input);
    auto ctx = make_context(t.access, events, cancel, backup, seen, allowed_tools,
                            t.delegates_tool_permissions, deadline);
    const auto started = std::chrono::steady_clock::now();
    try {
        out = t.run(input, ctx);
    } catch (const std::exception& e) {
        out = {std::string("Tool failed: ") + e.what(), true, "失败", {}};
    }
    out.elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - started);

    if (events.tool_finished) events.tool_finished(t, input, out);

    if (hooks_.post_tool && !cancel.cancelled()) {
        const auto post = hooks_.post_tool(t, input, out, cancel);
        if (events.notice && !post.warning.empty()) events.notice(post.warning);
        if (!post.allow && !post.message.empty()) out.text += "\n[PostToolUse hook] " + post.message;
        if (!post.additional_context.empty()) out.text += "\n" + post.additional_context;
    }
    if (!hook.additional_context.empty()) out.text += "\n" + hook.additional_context;

    return out;
}

tool_context agent::make_context(tool_access access, agent_events& events, cancel_token& cancel, turn_backup* backup,
                                 std::map<fs::path, file_stamp>& seen, std::span<const std::string> allowed_tools,
                                 bool delegates_tool_permissions,
                                 std::optional<std::chrono::steady_clock::time_point> deadline) {
    return {root_, &cancel, access == tool_access::read && !delegates_tool_permissions ? nullptr : backup,
            &seen, events.tool_progress, events.ask, allowed_tools, deadline,
            [this, access, delegates_tool_permissions, &events, &cancel, backup, &seen, allowed_tools]
            (std::string_view name, const json& input,
             std::optional<std::chrono::steady_clock::time_point> child_deadline) -> tool_output {
                if (cancel.cancelled()) return {"[Interrupted by the user]", true, "已取消", {}};
                if (name == "codemode") return {"CodeMode scripts cannot invoke codemode recursively.", true, "拒绝递归", {}};
                const auto builtin = find_tool(name);
                if (!builtin) return {"Tool is unavailable: " + std::string(name), true, "不可用", {}};
                if (!delegates_tool_permissions && builtin->access > access)
                    return {"Operation exceeds the tool's declared access.", true, "已拒绝", {}};
                return run_tool(*builtin, input, events, cancel, backup, seen, allowed_tools, child_deadline);
            }, events.notice,
            [this](std::string_view query, std::size_t limit, bool activate) {
                return tools_.search(query, limit, activate);
            },
            [this](std::string_view name) -> std::optional<tool_search_result> {
                const auto* found = find_tool(name);
                if (!found || found->name == "codemode") return std::nullopt;
                return tool_search_result{found->name, found->description, found->schema};
            },
            [this, &events, &cancel, backup, &seen, allowed_tools]
            (const std::vector<tool_invocation>& calls,
             std::optional<std::chrono::steady_clock::time_point> child_deadline) {
                return invoke_tool_batch(calls, events, cancel, *backup, seen, allowed_tools, child_deadline);
            }};
}

tool_output agent::run_local(const std::function<tool_output(tool_context&)>& action,
                             agent_events& events, cancel_token& cancel) {
    turn_backup backup;
    auto ctx = make_context(tool_access::execute, events, cancel, &backup, seen_, {});
    tool_output out;
    try {
        out = action(ctx);
    } catch (const std::exception& e) {
        out = {e.what(), true, "失败", {}};
    }
    if (!backup.originals.empty()) backups_.push_back(std::move(backup));
    return out;
}

void agent::set_extensions(tool_registry tools, agent_hooks hooks) {
    tools_ = std::move(tools);
    hooks_ = std::move(hooks);
}

std::vector<std::string> agent::undo(std::vector<std::string>* failed) {
    if (failed) failed->clear();
    if (backups_.empty()) return {};
    auto result = restore_report(backups_.back(), root_);
    if (failed) *failed = result.failed;
    if (result.failed.empty()) backups_.pop_back();
    else {
        turn_backup remaining;
        for (const auto& [path, original] : backups_.back().originals) {
            if (std::ranges::find(result.failed, display_path(root_, path)) != result.failed.end()) {
                remaining.originals.emplace(path, original);
            }
        }
        backups_.back() = std::move(remaining);
    }
    seen_.clear();
    pending_note_ = "[The user undid your file changes from an earlier turn. Restored: ";
    for (std::size_t i = 0; i < result.restored.size(); ++i) pending_note_ += (i ? ", " : "") + result.restored[i];
    pending_note_ += ". Re-read files before editing them.]";
    return result.restored;
}

turn_result agent::compact(agent_events& events, cancel_token& cancel, std::string_view focus) {
    if (messages.empty()) return {stop_reason::end_turn, {}};
    const auto build_started = std::chrono::steady_clock::now();
    auto r = request();
    r.tools.clear();
    std::string ask = "Summarise this conversation so that work can continue without it. Keep: the user's goals and "
                      "decisions, files read or changed and why, commands run and their outcomes, open problems and "
                      "next steps. Be specific with paths and names. Write plain text, no preamble.";
    if (!focus.empty()) ask += " Focus on: " + std::string(focus);
    r.messages.push_back(message{message::role::user, {block::make_text(ask)}});
    close_dangling_tool_calls(r.messages);
    auto reply = send(r, {}, events, cancel, build_started, true);
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
    return {stop_reason::end_turn, {}};
}

} // namespace ode
