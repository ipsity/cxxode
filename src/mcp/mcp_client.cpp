#include "mcp/mcp_client.hpp"
#include "config/config.hpp"
#include "core/session.hpp"

#include <sstream>

namespace lay {

mcp_client::mcp_client(const server_config& config) : config_(config) {}

mcp_client::~mcp_client() {
    if (process_ && process_->is_running()) {
        // Send shutdown notification (best effort)
        try {
            json shutdown_msg = {
                {"jsonrpc", "2.0"},
                {"method", "shutdown"},
                {"id", ++request_id_}
            };
            write_message(shutdown_msg.dump());
        } catch (...) {}
    }
}

bool mcp_client::initialize() {
    // Start the MCP server process
    interactive_process::options opts;
    opts.env = config_.env;

    process_ = std::make_unique<interactive_process>(config_.command, config_.args, opts);
    if (!process_->is_running()) return false;

    try {
        // Send initialize request
        json init_params = {
            {"protocolVersion", "2024-11-05"},
            {"capabilities", json::object()},
            {"clientInfo", {
                {"name", "cxxode"},
                {"version", "0.2.0"}
            }}
        };

        auto response = send_request("initialize", init_params);
        if (!response.contains("result")) return false;

        // Send initialized notification
        json initialized_msg = {
            {"jsonrpc", "2.0"},
            {"method", "initialized"}
        };
        write_message(initialized_msg.dump());

        return true;
    } catch (const std::exception&) {
        return false;
    }
}

std::vector<mcp_client::mcp_tool_info> mcp_client::list_tools() {
    std::vector<mcp_tool_info> tools;
    try {
        auto response = send_request("tools/list", json::object());
        if (response.contains("result") && response["result"].contains("tools")) {
            for (const auto& tool_json : response["result"]["tools"]) {
                mcp_tool_info info;
                info.name = tool_json.value("name", "");
                info.description = tool_json.value("description", "");
                info.schema = tool_json.value("inputSchema", json::object());
                if (!info.name.empty()) tools.push_back(std::move(info));
            }
        }
    } catch (const std::exception&) {
        // Server might not support tools/list
    }
    return tools;
}

tool_output mcp_client::call_tool(const std::string& name, const json& arguments) {
    try {
        json params = {
            {"name", name},
            {"arguments", arguments}
        };
        auto response = send_request("tools/call", params);

        if (response.contains("result")) {
            const auto& result = response["result"];
            // MCP returns {content: [{type, text}], isError}
            std::string text;
            if (result.contains("content") && result["content"].is_array()) {
                for (const auto& block : result["content"]) {
                    if (block.contains("text")) {
                        text += block["text"].get<std::string>();
                    }
                }
            }
            bool is_error = result.value("isError", false);
            return {text, is_error, is_error ? "失败" : "成功", {}};
        } else if (response.contains("error")) {
            const auto& error = response["error"];
            std::string msg = error.value("message", "MCP error");
            return {msg, true, "错误", {}};
        }
    } catch (const std::exception& e) {
        return {e.what(), true, "异常", {}};
    }
    return {"No response", true, "无响应", {}};
}

json mcp_client::send_request(const std::string& method, const json& params) {
    json request = {
        {"jsonrpc", "2.0"},
        {"method", method},
        {"params", params},
        {"id", ++request_id_}
    };

    write_message(request.dump());
    std::string response_str = read_message();
    return json::parse(response_str);
}

std::string mcp_client::read_message() {
    // JSON-RPC over stdio uses Content-Length header
    // Format: Content-Length: N\r\n\r\n{json}

    if (!process_ || !process_->is_running()) throw std::runtime_error("Process not running");

    // Read until we find Content-Length header
    std::string header;
    char c;
    while (process_->read_stdout(&c, 1) == 1) {
        header += c;
        if (header.size() >= 4 && header.substr(header.size() - 4) == "\r\n\r\n") break;
    }

    // Parse Content-Length
    std::size_t length = 0;
    const auto prefix = header.find("Content-Length:");
    if (prefix != std::string::npos) {
        std::istringstream ss(header.substr(prefix + 15));
        ss >> length;
    }

    if (length == 0 || length > 10 * 1024 * 1024) {
        throw std::runtime_error("Invalid Content-Length");
    }

    // Read the JSON body
    std::string body(length, '\0');
    std::size_t read = 0;
    while (read < length) {
        const auto n = process_->read_stdout(&body[read], length - read);
        if (n <= 0) throw std::runtime_error("Read failed");
        read += n;
    }

    return body;
}

void mcp_client::write_message(const std::string& message) {
    if (!process_ || !process_->is_running()) throw std::runtime_error("Process not running");

    std::ostringstream header;
    header << "Content-Length: " << message.size() << "\r\n\r\n";

    const auto header_str = header.str();
    process_->write_stdin(header_str.data(), header_str.size());
    process_->write_stdin(message.data(), message.size());
}

std::vector<mcp_client::server_config> load_mcp_servers(const fs::path& workspace) {
    std::vector<mcp_client::server_config> servers;

    // Try .mcp.json in workspace
    const auto mcp_json = workspace / ".mcp.json";
    if (fs::exists(mcp_json)) {
        try {
            const auto content = read_file(mcp_json);
            if (content) {
                auto config = json::parse(*content);
                if (config.contains("mcpServers") && config["mcpServers"].is_object()) {
                    for (const auto& [name, server] : config["mcpServers"].items()) {
                        mcp_client::server_config cfg;
                        cfg.name = name;
                        cfg.command = server.value("command", "");
                        cfg.args = server.value("args", std::vector<std::string>{});
                        if (server.contains("env") && server["env"].is_object()) {
                            for (const auto& [k, v] : server["env"].items()) {
                                cfg.env[k] = v.get<std::string>();
                            }
                        }
                        if (!cfg.command.empty()) servers.push_back(cfg);
                    }
                }
            }
        } catch (const std::exception&) {
            // Invalid .mcp.json, skip
        }
    }

    return servers;
}

} // namespace lay
