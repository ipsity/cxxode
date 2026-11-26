#include "app/rpc.hpp"
#include "core/session.hpp"
#include "ui/term.hpp"

#include <iostream>
#include <sstream>

namespace ode {

void rpc_server::run() {
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) continue;

        try {
            auto req = json::parse(line);
            if (!req.is_object() || !req.contains("method") || !req.contains("id")) {
                send_error(-1, "Invalid request: missing 'method' or 'id'");
                continue;
            }
            handle_request(req);
        } catch (const std::exception& e) {
            send_error(-1, std::string("Parse error: ") + e.what());
        }
    }
}

void rpc_server::handle_request(const json& req) {
    const int id = req["id"];
    const std::string method = req["method"];
    const auto params = req.value("params", json::object());
    agent_events events;
    events.request_finished = [this, id](const request_timing& timing) {
        send_event(id, "request_finished", {
            {"build_ms", timing.build_elapsed.count()},
            {"elapsed_ms", timing.elapsed.count()},
            {"encode_ms", timing.provider ? json(timing.provider->encode.count()) : json(nullptr)},
            {"http_ms", timing.provider ? json(timing.provider->http.count()) : json(nullptr)},
            {"first_output_ms", timing.first_output ? json(timing.first_output->count()) : json(nullptr)},
            {"compact", timing.compact}
        });
    };

    if (method == "run") {
        const std::string prompt = params.value("prompt", std::string{});
        if (prompt.empty()) {
            send_error(id, "Missing 'prompt' parameter");
            return;
        }

        events.text = [this, id](std::string_view text) {
            send_event(id, "text", {{"text", std::string(text)}});
        };
        events.notice = [this, id](std::string_view text) {
            send_event(id, "notice", {{"text", std::string(text)}});
        };
        events.thinking = [this, id](std::string_view text, std::string_view sig) {
            send_event(id, "thinking", {{"text", std::string(text)}, {"signature", std::string(sig)}});
        };
        events.tool_started = [this, id](const tool& t, const json& input) {
            send_event(id, "tool_started", {{"tool", t.name}, {"input", input}});
        };
        events.tool_finished = [this, id](const tool& t, const json& input, const tool_output& out) {
            send_event(id, "tool_finished", {
                {"tool", t.name},
                {"input", input},
                {"output", out.text},
                {"is_error", out.is_error},
                {"elapsed_ms", out.elapsed ? json(out.elapsed->count()) : json(nullptr)}
            });
        };
        events.usage_updated = [this, id](const usage& u) {
            send_event(id, "usage", {
                {"input", u.input},
                {"output", u.output},
                {"cache_read", u.cache_read},
                {"cache_write", u.cache_write}
            });
        };
        events.ask = [this, id](const tool& t, const json& input) {
            // Send ask request and wait for response
            send_event(id, "ask", {{"tool", t.name}, {"input", input}});

            // Read response from client
            std::string line;
            if (std::getline(std::cin, line)) {
                try {
                    auto resp = json::parse(line);
                    if (resp.contains("answer")) {
                        const std::string ans = resp["answer"];
                        if (ans == "yes") return answer::yes;
                        if (ans == "always") return answer::always;
                    }
                } catch (...) {}
            }
            return answer::no;
        };

        const auto result = core.run(prompt, events, cancel);
        send_event(id, "done", {
            {"stop", [&] {
                switch (result.stop) {
                    case stop_reason::end_turn: return "end_turn";
                    case stop_reason::max_tokens: return "max_tokens";
                    case stop_reason::cancelled: return "cancelled";
                    case stop_reason::error: return "error";
                    case stop_reason::refusal: return "refusal";
                    default: return "unknown";
                }
            }()},
            {"error", result.error}
        });

    } else if (method == "interrupt") {
        cancel.cancel();
        send_event(id, "done", {{"stop", "cancelled"}, {"error", ""}});

    } else if (method == "undo") {
        std::vector<std::string> failed;
        const auto restored = core.undo(&failed);
        send_event(id, "undo", {{"restored", restored}, {"failed", failed}});

    } else if (method == "compact") {
        const auto result = core.compact(events, cancel);
        send_event(id, "done", {{"stop", result.stop == stop_reason::end_turn ? "end_turn" : "error"}});

    } else {
        send_error(id, "Unknown method: " + method);
    }
}

void rpc_server::send_event(int id, std::string_view event, const json& data) {
    json msg = {
        {"id", id},
        {"event", event},
        {"data", data}
    };
    std::cout << msg.dump() << "\n" << std::flush;
}

void rpc_server::send_error(int id, std::string_view message) {
    send_event(id, "error", {{"message", std::string(message)}});
}

} // namespace ode
