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

agent::agent(provider& model_provider, std::vector<tool> tools, permission_rules& rules, fs::path root)
    : provider_(model_provider), tools_(std::move(tools)), rules_(rules), root_(std::move(root)) {}

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

turn_result agent::run(std::string user_text, agent_events& events, cancel_token& cancel) {
    message user{message::role::user, {}};
    if (!pending_note_.empty()) {
        user.blocks.push_back(block::make_text(pending_note_));
        pending_note_.clear();
    }
    user.blocks.push_back(block::make_text(std::move(user_text)));
    append(std::move(user));
    turn_backup backup;
    const auto keep_backup = [&] {
        if (!backup.originals.empty()) backups_.push_back(std::move(backup));
        backup = {};
    };

    for (int round = 0; round < max_rounds_without_user; ++round) {
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
            if (steering.empty() || cancel.cancelled()) {
                keep_backup();
                return {reply.stop, stop_text(reply.stop)};
            }
            message next{message::role::user, {}};
            for (auto& s : steering) next.blocks.push_back(block::make_text(std::move(s)));
            append(std::move(next));
            continue;
        }

        message results{message::role::user, {}};
        run_tools(reply.content, events, cancel, results, backup);
        if (events.take_steering) {
            for (auto& s : events.take_steering()) steering.push_back(std::move(s));
        }
        for (auto& s : steering) {
            results.blocks.push_back(block::make_text("[The user added while you were working]\n" + s));
        }
        append(std::move(results));
        if (cancel.cancelled()) {
            keep_backup();
            return {stop_reason::cancelled, "Cancelled."};
        }
    }
    keep_backup();
    return {stop_reason::error, "Stopped after " + std::to_string(max_rounds_without_user) + " rounds without user input."};
}

void agent::run_tools(const message& reply, agent_events& events, cancel_token& cancel, message& results,
                      turn_backup& backup) {
    const auto calls = reply.tool_uses();
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
            auto out = run_one(*calls[i], events, cancel, backup, seen_);
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
            std::vector<std::map<fs::path, file_stamp>> stamps(end - i);
            std::vector<std::future<tool_output>> pending;
            for (std::size_t k = i; k < end; ++k) {
                pending.push_back(std::async(std::launch::async, [&, k] {
                    return run_one(*calls[k], serial, cancel, backup, stamps[k - i]);
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
                           std::map<fs::path, file_stamp>& seen) {
    const auto* t = find_tool(call.name);
    if (!t) return {"Unknown tool: " + call.name, true, "未知工具", {}};
    if (!call.input.is_object()) return {"Tool input must be a JSON object.", true, "参数错误", {}};
    tool_output out;
    switch (rules_.check(*t, call.input, root_)) {
        case verdict::deny:
            out = {rules_.mode() == permission_mode::plan
                       ? "Plan mode is read-only. Describe the change instead; the user can switch modes with Shift+Tab."
                       : "Denied by a permission rule.",
                   true, "已拒绝", {}};
            if (events.tool_started) events.tool_started(*t, call.input);
            if (events.tool_finished) events.tool_finished(*t, call.input, out);
            return out;
        case verdict::ask: {
            const auto a = events.ask ? events.ask(*t, call.input) : answer::no;
            if (a == answer::no) {
                out = {"The user declined this tool call. Ask what they want instead, or try another approach.", true, "已拒绝", {}};
                if (events.tool_started) events.tool_started(*t, call.input);
                if (events.tool_finished) events.tool_finished(*t, call.input, out);
                return out;
            }
            if (a == answer::always) rules_.allow.push_back(permission_rules::always_rule(*t, call.input));
            break;
        }
        case verdict::allow: break;
    }
    if (events.tool_started) events.tool_started(*t, call.input);
    tool_context ctx{root_, &cancel, t->access == tool_access::write ? &backup : nullptr, &seen, events.tool_progress};
    try {
        out = t->run(call.input, ctx);
    } catch (const std::exception& e) {
        out = {std::string("Tool failed: ") + e.what(), true, "失败", {}};
    }
    if (events.tool_finished) events.tool_finished(*t, call.input, out);
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
    if (!reply.complete) return {cancel.cancelled() ? stop_reason::cancelled : stop_reason::error, reply.error};
    message summary{message::role::user,
                    {block::make_text("[Summary of the earlier conversation]\n" + reply.content.plain_text())}};
    if (log) log->append({{"v", 1}, {"type", "compact"}, {"message", to_json(summary)}});
    messages = {summary, message{message::role::assistant, {block::make_text("Understood. I'll continue from the summary.")}}};
    if (log) log->append_message(messages.back());
    context_tokens = reply.tokens.output;
    (void)events;
    return {stop_reason::end_turn, {}};
}

} // namespace ode
