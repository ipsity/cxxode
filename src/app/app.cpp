#include "app/app.hpp"
#include "provider/models.hpp"
#include "app/interactive.hpp"
#include "ui/term.hpp"
#include "ui/welcome.hpp"

namespace ode {

std::optional<std::string> app::use_provider(const std::string& name) {
    provider_config resolved;
    if (auto error = resolve_provider(config, name, resolved)) {
        if (name == config.provider) credential_error = *error;
        return error;
    }
    const bool changed = name != config.provider;
    config.provider = name;
    // The main agent and Task tools share the client, which reads this config on each request.
    provider_cfg = std::move(resolved);
    if (!model_provider) model_provider = make_provider(provider_cfg);
    credential_error.reset();
    if (changed && core) set_model(configured_model(config, name));
    return std::nullopt;
}

void app::refresh_credentials() {
    (void)use_provider(config.provider);
}

bool app::open_session(const std::optional<fs::path>& resume) {
    log.close();
    if (resume) {
        auto loaded = load_session(*resume);
        core->messages = std::move(loaded.messages);
        session_id = resume->stem().string();
        log.open(*resume);
    } else {
        core->messages.clear();
        session_id = new_session_id();
        log.open(sessions_dir(root) / (session_id + ".jsonl"));
        log.append({{"v", 1}, {"type", "meta"}, {"model", core->model}, {"cwd", root.generic_string()}});
    }
    core->context_tokens = 0;
    core->log = log.is_open() ? &log : nullptr;
    return log.is_open();
}

void app::set_model(const std::string& requested, std::size_t context_window) {
    const auto model = resolve_model_alias(requested);
    core->model = model;
    if (context_window == 0) context_window = known_model_context_window(provider_cfg, model);
    core->context_window = context_window != 0 ? context_window : default_context_window;
    if (log.is_open()) log.append({{"v", 1}, {"type", "meta"}, {"model", model}});
}

int run_interactive(app& a, std::string first_prompt) {
    ui::terminal term;
    interactive_ui ui(a, term);
    return ui.run(std::move(first_prompt));
}

int run_print(app& a, std::string prompt) {
    agent_events ev;
    bool thinking = false;
    bool thinking_line_ended = false;
    const auto finish_thinking = [&] {
        if (thinking && !thinking_line_ended) ui::write_stream(true, "\n");
        thinking = false;
    };
    ev.request_started = finish_thinking;
    ev.notice = [&](std::string_view message) {
        finish_thinking();
        ui::write_stream(true, std::string(message) + '\n');
    };
    ev.compacted = [&] { ev.notice("上下文已自动压缩"); };
    ev.thinking = [&](std::string_view t, std::string_view) {
        if (t.empty()) return;
        if (!thinking) ui::write_stream(true, "✻ 思考\n");
        ui::write_stream(true, t);
        thinking = true;
        thinking_line_ended = t.ends_with('\n');
    };
    ev.text = [&](std::string_view t) {
        if (t.empty()) return;
        finish_thinking();
        ui::write_stream(false, t);
    };
    ev.tool_started = [&](const tool& t, const json& in) {
        finish_thinking();
        ui::write_stream(true, "● " + t.name + " " + tool_label(t, in, a.root) + "\n");
    };
    // Headless runs cannot ask; anything that would need a prompt is declined.
    ev.ask = [](const tool&, const json&) { return answer::no; };
    cancel_token cancel;
    const auto r = a.core->run(std::move(prompt), ev, cancel);
    finish_thinking();
    ui::write_stream(false, "\n");
    if (r.stop == stop_reason::error || r.stop == stop_reason::cancelled) {
        ui::write_stream(true, "ode: " + (r.error.empty() ? std::string("stopped") : r.error) + "\n");
        return 1;
    }
    return 0;
}

} // namespace ode
