#include "app/app.hpp"
#include "app/interactive.hpp"
#include "ui/term.hpp"
#include "ui/welcome.hpp"

namespace ode {

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
    core->log = log.is_open() ? &log : nullptr;
    return log.is_open();
}

void app::set_model(const std::string& requested) {
    const auto model = resolve_model_alias(requested);
    core->model = model;
    // Opus/Sonnet 4.x and later accept a 1M window only on request; 200k is the safe default.
    core->context_window = model.find("[1m]") != std::string::npos ? 1000000 : 200000;
    if (log.is_open()) log.append({{"v", 1}, {"type", "meta"}, {"model", model}});
}

int run_interactive(app& a, std::string first_prompt) {
    ui::terminal term;
    interactive_ui ui(a, term);
    return ui.run(std::move(first_prompt));
}

int run_print(app& a, std::string prompt) {
    agent_events ev;
    ev.text = [](std::string_view t) { ui::write_stream(false, t); };
    ev.tool_started = [&](const tool& t, const json& in) {
        ui::write_stream(true, "● " + t.name + " " + tool_label(t, in, a.root) + "\n");
    };
    // Headless runs cannot ask; anything that would need a prompt is declined.
    ev.ask = [](const tool&, const json&) { return answer::no; };
    cancel_token cancel;
    const auto r = a.core->run(std::move(prompt), ev, cancel);
    ui::write_stream(false, "\n");
    if (r.stop == stop_reason::error || r.stop == stop_reason::cancelled) {
        ui::write_stream(true, "ode: " + (r.error.empty() ? std::string("stopped") : r.error) + "\n");
        return 1;
    }
    return 0;
}

} // namespace ode
