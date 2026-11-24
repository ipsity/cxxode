#pragma once

#include "core/agent.hpp"
#include "tools/tool.hpp"

#include <functional>
#include <string>

namespace ode {

// RPC protocol for embedding cxxode in editors and other tools.
// Uses JSONL (JSON Lines) over stdin/stdout for communication.
//
// Request format (from client):
// {"id": 1, "method": "run", "params": {"prompt": "..."}}
// {"id": 2, "method": "continue", "params": {"text": "..."}}
// {"id": 3, "method": "interrupt", "params": {}}
// {"id": 4, "method": "undo", "params": {}}
//
// Event format (to client):
// {"id": 1, "event": "text", "data": "..."}
// {"id": 1, "event": "thinking", "data": {"text": "...", "signature": "..."}}
// {"id": 1, "event": "tool_started", "data": {"tool": "...", "input": {...}}}
// {"id": 1, "event": "tool_finished", "data": {"tool": "...", "output": "...", "is_error": false}}
// {"id": 1, "event": "done", "data": {"stop": "end_turn", "error": ""}}
// {"id": 1, "event": "error", "data": {"message": "..."}}

struct rpc_server {
    agent& core;
    cancel_token& cancel;

    // Run the RPC server loop (blocks until stdin closes or error)
    void run();

private:
    void handle_request(const json& req);
    void send_event(int id, std::string_view event, const json& data);
    void send_error(int id, std::string_view message);
};

} // namespace ode
