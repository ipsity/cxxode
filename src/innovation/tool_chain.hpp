#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <memory>

namespace ode {

/**
 * 工具组合系统
 * Week 4 Day 22: 创新功能 - 工具链
 *
 * 功能：
 * - 定义工具执行链
 * - 自动化工作流
 * - 依赖管理
 */

struct tool_chain_node {
    std::string tool_name;
    std::map<std::string, std::string> params;  // 工具参数
    std::vector<std::string> depends_on;        // 依赖的节点 ID
    std::string output_key;                     // 输出键名

    tool_chain_node(std::string name) : tool_name(std::move(name)) {}
};

class tool_chain {
public:
    tool_chain() = default;

    // 添加节点
    tool_chain& add(const std::string& id, tool_chain_node node) {
        nodes_[id] = std::move(node);
        return *this;
    }

    // 链式添加（自动依赖前一个）
    tool_chain& then(const std::string& id, const std::string& tool_name) {
        tool_chain_node node(tool_name);
        if (!nodes_.empty()) {
            node.depends_on.push_back(last_id_);
        }
        nodes_[id] = std::move(node);
        last_id_ = id;
        return *this;
    }

    // 设置参数
    tool_chain& param(const std::string& key, const std::string& value) {
        if (!last_id_.empty()) {
            nodes_[last_id_].params[key] = value;
        }
        return *this;
    }

    // 设置输出键
    tool_chain& output(const std::string& key) {
        if (!last_id_.empty()) {
            nodes_[last_id_].output_key = key;
        }
        return *this;
    }

    // 获取所有节点
    const std::map<std::string, tool_chain_node>& nodes() const { return nodes_; }

    // 获取执行顺序（拓扑排序）
    std::vector<std::string> execution_order() const {
        std::vector<std::string> order;
        std::map<std::string, bool> visited;

        for (const auto& [id, _] : nodes_) {
            visit(id, visited, order);
        }

        return order;
    }

    // 验证链的有效性
    bool validate() const {
        // 检查循环依赖
        for (const auto& [id, node] : nodes_) {
            if (has_cycle(id, {})) {
                return false;
            }
        }
        return true;
    }

private:
    void visit(const std::string& id, std::map<std::string, bool>& visited,
               std::vector<std::string>& order) const {
        if (visited[id]) return;

        const auto& node = nodes_.at(id);
        for (const auto& dep : node.depends_on) {
            if (nodes_.count(dep)) {
                visit(dep, visited, order);
            }
        }

        visited[id] = true;
        order.push_back(id);
    }

    bool has_cycle(const std::string& id, std::vector<std::string> path) const {
        if (std::find(path.begin(), path.end(), id) != path.end()) {
            return true;  // 发现循环
        }

        path.push_back(id);
        const auto& node = nodes_.at(id);
        for (const auto& dep : node.depends_on) {
            if (nodes_.count(dep) && has_cycle(dep, path)) {
                return true;
            }
        }

        return false;
    }

    std::map<std::string, tool_chain_node> nodes_;
    std::string last_id_;
};

/**
 * 工具链构建器（流式API）
 */
class chain_builder {
public:
    static tool_chain read_edit_write(const std::string& file) {
        return tool_chain()
            .then("read", "Read").param("file", file).output("content")
            .then("edit", "Edit").param("content", "$content")
            .then("write", "Write").param("file", file);
    }

    static tool_chain search_and_read(const std::string& pattern) {
        return tool_chain()
            .then("search", "Grep").param("pattern", pattern).output("files")
            .then("read", "Read").param("file", "$files");
    }
};

} // namespace ode
