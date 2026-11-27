#pragma once

#include "platform/interactive_process.hpp"
#include "tools/tool.hpp"

#include <chrono>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ode {

// MCP client for one stdio server. Messages are newline-delimited JSON-RPC, which is what
// the spec requires; Content-Length framing is the language-server format and no server speaks it.
class mcp_client {
public:
    struct server_config {
        std::string name;
        std::string command;
        std::vector<std::string> args;
        std::map<std::string, std::string> env;
        fs::path working_dir;
        tool_exposure exposure = tool_exposure::deferred;
    };

    struct mcp_tool_info {
        std::string name;
        std::string description;
        json schema;
    };

    explicit mcp_client(server_config config);
    ~mcp_client();

    mcp_client(const mcp_client&) = delete;
    mcp_client& operator=(const mcp_client&) = delete;

    // Starts the process and does the initialize handshake. On failure the process is gone
    // and error() says why.
    bool initialize(std::chrono::milliseconds timeout = std::chrono::seconds(30));

    std::vector<mcp_tool_info> list_tools();
    tool_output call_tool(const std::string& name, const json& arguments);

    const std::string& server_name() const { return config_.name; }
    tool_exposure exposure() const { return config_.exposure; }
    const std::string& error() const { return error_; }
    bool is_running() const { return process_ && process_->is_running(); }

private:
    json send_request(const std::string& method, const json& params, std::chrono::milliseconds timeout);
    json read_response(int id, std::chrono::milliseconds timeout);
    void write_message(const json& message);

    server_config config_;
    std::unique_ptr<interactive_process> process_;
    std::string pending_;  // bytes read past the end of the last message
    int request_id_ = 0;
    std::string error_;
};

// Every tool of every running server, named mcp__<server>__<tool>.
std::vector<tool> make_mcp_tools(const std::vector<std::shared_ptr<mcp_client>>& clients);

// Hidden stdio server used by the tests. Speaks newline-delimited JSON-RPC on stdin/stdout
// and exits when stdin closes. Not a user-facing command.
int run_mcp_test_server();

} // namespace ode
