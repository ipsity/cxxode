#pragma once

#include "tools/tool.hpp"
#include "platform/interactive_process.hpp"

#include <map>
#include <memory>
#include <string>
#include <vector>

namespace ode {

// MCP (Model Context Protocol) client for stdio-based servers.
// Implements JSON-RPC 2.0 over stdin/stdout to communicate with external tools.
class mcp_client {
public:
    struct server_config {
        std::string name;           // Server identifier
        std::string command;        // Executable path
        std::vector<std::string> args;
        std::map<std::string, std::string> env;
    };

    struct mcp_tool_info {
        std::string name;
        std::string description;
        json schema;
    };

    explicit mcp_client(const server_config& config);
    ~mcp_client();

    // Start the MCP server process and perform initialization handshake
    bool initialize();

    // List available tools from the server
    std::vector<mcp_tool_info> list_tools();

    // Call a tool on the server
    tool_output call_tool(const std::string& name, const json& arguments);

    // Server name for tool prefixing (mcp__servername__toolname)
    const std::string& server_name() const { return config_.name; }

    bool is_running() const { return process_ && process_->is_running(); }

private:
    json send_request(const std::string& method, const json& params);
    std::string read_message();
    void write_message(const std::string& message);

    server_config config_;
    std::unique_ptr<interactive_process> process_;
    int request_id_ = 0;
};

// Load MCP server configurations from settings
std::vector<mcp_client::server_config> load_mcp_servers(const fs::path& workspace);

// Every tool of every running server, named mcp__<server>__<tool>.
std::vector<tool> make_mcp_tools(const std::vector<std::shared_ptr<mcp_client>>& clients);

} // namespace ode
