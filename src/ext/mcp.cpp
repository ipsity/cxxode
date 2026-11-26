#include "ext/mcp.hpp"
#include "base/utf.hpp"
#include "core/version.hpp"

#include <iostream>
#include <stdexcept>

namespace ode {
namespace {

constexpr std::size_t max_message = 16 * 1024 * 1024;

// The spec allows the server to write logging and other notifications at any time, so a reply
// is whichever line carries the id we asked for, not simply the next line.
json take_response(std::string& buffer, int id) {
    while (true) {
        const auto end = buffer.find('\n');
        if (end == std::string::npos) return {};
        std::string line = buffer.substr(0, end);
        buffer.erase(0, end + 1);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;
        if (line.size() > max_message) throw std::runtime_error("MCP message exceeds 16 MB");

        auto message = json::parse(line, nullptr, false);
        if (message.is_discarded() || !message.is_object()) continue;
        if (!message.contains("id") || !message["id"].is_number_integer() || message["id"].get<int>() != id) continue;
        return message;
    }
}

} // namespace

mcp_client::mcp_client(server_config config) : config_(std::move(config)) {}

mcp_client::~mcp_client() = default;

bool mcp_client::initialize(std::chrono::milliseconds timeout) {
    error_.clear();
    try {
        interactive_process::options opts;
        opts.env = config_.env;
        opts.working_dir = config_.working_dir;
        process_ = std::make_unique<interactive_process>(config_.command, config_.args, opts);
    } catch (const std::exception& e) {
        error_ = e.what();
        return false;
    }

    try {
        const json params = {
            {"protocolVersion", "2024-11-05"},
            {"capabilities", json::object()},
            {"clientInfo", {{"name", "ode"}, {"version", std::string(version())}}},
        };
        const auto response = send_request("initialize", params, timeout);
        if (response.contains("error")) {
            error_ = response["error"].value("message", "initialize rejected");
            process_.reset();
            return false;
        }
        if (!response.contains("result")) {
            error_ = "initialize returned no result";
            process_.reset();
            return false;
        }
        // A notification, so it has no id and expects no reply.
        write_message({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
        return true;
    } catch (const std::exception& e) {
        error_ = e.what();
        process_.reset();
        return false;
    }
}

std::vector<mcp_client::mcp_tool_info> mcp_client::list_tools() {
    std::vector<mcp_tool_info> tools;
    json cursor;
    // tools/list is paginated; an absent nextCursor is the last page.
    for (int page = 0; page < 64; ++page) {
        json params = json::object();
        if (!cursor.is_null()) params["cursor"] = cursor;
        const auto response = send_request("tools/list", params, std::chrono::seconds(30));
        if (response.contains("error") || !response.contains("result")) break;
        const auto& result = response["result"];
        if (result.contains("tools") && result["tools"].is_array()) {
            for (const auto& tool_json : result["tools"]) {
                mcp_tool_info info;
                info.name = tool_json.value("name", "");
                info.description = tool_json.value("description", "");
                info.schema = tool_json.value("inputSchema", json::object());
                if (!info.name.empty()) tools.push_back(std::move(info));
            }
        }
        if (!result.contains("nextCursor") || !result["nextCursor"].is_string()) break;
        cursor = result["nextCursor"];
    }
    return tools;
}

tool_output mcp_client::call_tool(const std::string& name, const json& arguments,
                                 std::optional<std::chrono::steady_clock::time_point> deadline) {
    std::lock_guard lock(call_mutex_);
    try {
        auto timeout = std::chrono::milliseconds(120000);
        if (deadline) {
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(*deadline - std::chrono::steady_clock::now());
            if (remaining <= std::chrono::milliseconds::zero()) return {"CodeMode execution deadline exceeded", true, "timeout", {}};
            timeout = std::min(timeout, remaining);
        }
        const auto response = send_request("tools/call", {{"name", name}, {"arguments", arguments}},
                                           timeout);
        if (response.contains("error")) {
            const auto message = response["error"].value("message", "MCP error");
            return {message, true, "错误", {}};
        }
        if (!response.contains("result")) return {"No response", true, "无响应", {}};

        // Result content is a list of typed blocks; only text blocks have a string form.
        const auto& result = response["result"];
        std::string text;
        if (result.contains("content") && result["content"].is_array()) {
            for (const auto& block : result["content"]) {
                if (block.value("type", "") == "text" && block.contains("text") && block["text"].is_string()) {
                    text += block["text"].get<std::string>();
                }
            }
        }
        const bool is_error = result.value("isError", false);
        return {text, is_error, is_error ? "失败" : "成功", {}};
    } catch (const std::exception& e) {
        return {e.what(), true, "异常", {}};
    }
}

json mcp_client::send_request(const std::string& method, const json& params, std::chrono::milliseconds timeout) {
    const int id = ++request_id_;
    write_message({{"jsonrpc", "2.0"}, {"method", method}, {"params", params}, {"id", id}});
    return read_response(id, timeout);
}

json mcp_client::read_response(int id, std::chrono::milliseconds timeout) {
    if (!process_) throw std::runtime_error("MCP server is not running");
    if (const auto ready = take_response(pending_, id); !ready.is_null()) return ready;

    const auto deadline = std::chrono::steady_clock::now() + timeout;
    char buffer[8192];
    while (true) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
        if (left.count() <= 0) throw std::runtime_error("MCP server timed out");
        const auto n = process_->read_stdout(buffer, sizeof(buffer), left);
        if (n == 0) {
            throw std::runtime_error(process_->is_running() ? "MCP server timed out" : "MCP server exited");
        }
        pending_.append(buffer, n);
        if (pending_.size() > max_message) throw std::runtime_error("MCP message exceeds 16 MB");
        if (const auto ready = take_response(pending_, id); !ready.is_null()) return ready;
    }
}

void mcp_client::write_message(const json& message) {
    if (!process_ || !process_->is_running()) throw std::runtime_error("MCP server is not running");
    // Embedded newlines would split one message into several, so they must not survive dump().
    auto line = message.dump();
    line.push_back('\n');
    std::size_t sent = 0;
    while (sent < line.size()) {
        const auto n = process_->write_stdin(line.data() + sent, line.size() - sent);
        if (n == 0) throw std::runtime_error("MCP write failed");
        sent += n;
    }
}

std::vector<tool> make_mcp_tools(const std::vector<std::shared_ptr<mcp_client>>& clients) {
    std::vector<tool> tools;
    for (const auto& client : clients) {
        if (!client || !client->is_running()) continue;
        for (const auto& mcp_tool : client->list_tools()) {
            tool t;
            t.name = "mcp__" + client->server_name() + "__" + mcp_tool.name;
            t.description = mcp_tool.description;
            if (!client->server_name().empty()) t.description += " (MCP server: " + client->server_name() + ")";
            t.schema = mcp_tool.schema.is_object() ? mcp_tool.schema : json::object();
            t.access = tool_access::execute;
            t.exposure = client->exposure();
            t.label = [name = mcp_tool.name](const json&) { return name; };

            const auto original_name = mcp_tool.name;
            t.run = [client, original_name](const json& input, tool_context& context) {
                return client->call_tool(original_name, input, context.deadline);
            };
            tools.push_back(std::move(t));
        }
    }
    return tools;
}

int run_mcp_test_server() {
    // cin blocks in 4 KB units on a pipe, so the server would never see a short request
    // until the client closed stdin. ReadFile returns as soon as a line is available.
    HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    std::string buffer;
    char chunk[1024];
    DWORD got = 0;
    while (ReadFile(in, chunk, sizeof(chunk), &got, nullptr) && got > 0) {
        buffer.append(chunk, got);
        std::size_t end = 0;
        while ((end = buffer.find('\n')) != std::string::npos) {
            std::string line = buffer.substr(0, end);
            buffer.erase(0, end + 1);
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) continue;
            const auto message = json::parse(line, nullptr, false);
            if (message.is_discarded() || !message.is_object() || !message.contains("id")) continue;

            const auto method = message.value("method", "");
            json result;
            if (method == "initialize") {
                result = {{"protocolVersion", "2024-11-05"},
                          {"capabilities", {{"tools", json::object()}}},
                          {"serverInfo", {{"name", "ode-test"}, {"version", "0"}}}};
            } else if (method == "tools/list") {
                json echo = {{"name", "echo"},
                             {"description", "Echoes its argument"},
                             {"inputSchema", {{"type", "object"}}}};
                result = {{"tools", json::array({echo})}};
            } else if (method == "tools/call") {
                const auto args = message.value("params", json::object()).value("arguments", json::object());
                result = {{"content", json::array({{{"type", "text"}, {"text", args.dump()}}})},
                          {"isError", false}};
            } else {
                std::cout << json{{"jsonrpc", "2.0"},
                                  {"id", message["id"]},
                                  {"error", {{"code", -32601}, {"message", "unknown method"}}}}
                                 .dump()
                          << '\n'
                          << std::flush;
                continue;
            }
            std::cout << json{{"jsonrpc", "2.0"}, {"id", message["id"]}, {"result", result}}.dump() << '\n' << std::flush;
        }
    }
    return 0;
}

} // namespace ode
