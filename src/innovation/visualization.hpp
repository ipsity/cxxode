#pragma once

#include "innovation/tool_chain.hpp"
#include <string>
#include <sstream>

namespace ode {

/**
 * 工具流可视化
 * Week 4 Day 26: 创新功能 - 可视化
 *
 * 功能：
 * - 生成工具流图
 * - DOT 格式（Graphviz）
 * - ASCII 艺术图
 */

class flow_visualizer {
public:
    // 生成 DOT 格式（可用 Graphviz 渲染）
    static std::string to_dot(const tool_chain& chain) {
        std::ostringstream ss;
        ss << "digraph ToolChain {\n";
        ss << "  rankdir=LR;\n";
        ss << "  node [shape=box, style=rounded];\n\n";

        for (const auto& [id, node] : chain.nodes()) {
            ss << "  " << id << " [label=\"" << node.tool_name << "\"];\n";
        }

        ss << "\n";

        for (const auto& [id, node] : chain.nodes()) {
            for (const auto& dep : node.depends_on) {
                ss << "  " << dep << " -> " << id << ";\n";
            }
        }

        ss << "}\n";
        return ss.str();
    }

    // 生成 ASCII 艺术图
    static std::string to_ascii(const tool_chain& chain) {
        std::ostringstream ss;
        auto order = chain.execution_order();

        for (size_t i = 0; i < order.size(); ++i) {
            const auto& id = order[i];
            const auto& node = chain.nodes().at(id);

            ss << "[" << node.tool_name << "]";

            if (i < order.size() - 1) {
                ss << " -> ";
            }
        }

        return ss.str();
    }

    // 生成执行追踪
    static std::string trace_execution(
        const tool_chain& chain,
        const std::vector<std::string>& completed) {

        std::ostringstream ss;
        ss << "Execution Trace:\n";

        auto order = chain.execution_order();
        for (size_t i = 0; i < order.size(); ++i) {
            const auto& id = order[i];
            const auto& node = chain.nodes().at(id);

            bool is_completed = std::find(completed.begin(), completed.end(), id)
                             != completed.end();

            ss << "  " << (i + 1) << ". " << node.tool_name;

            if (is_completed) {
                ss << " ✓";
            } else {
                ss << " ⏸";
            }

            ss << "\n";
        }

        return ss.str();
    }

    // 生成详细的流程图（带参数）
    static std::string to_detailed_ascii(const tool_chain& chain) {
        std::ostringstream ss;
        auto order = chain.execution_order();

        ss << "Tool Chain Flow:\n";
        ss << "================\n\n";

        for (size_t i = 0; i < order.size(); ++i) {
            const auto& id = order[i];
            const auto& node = chain.nodes().at(id);

            ss << "[" << (i + 1) << "] " << node.tool_name << "\n";

            if (!node.params.empty()) {
                ss << "    Params:\n";
                for (const auto& [key, value] : node.params) {
                    ss << "      " << key << " = " << value << "\n";
                }
            }

            if (!node.output_key.empty()) {
                ss << "    Output: " << node.output_key << "\n";
            }

            if (!node.depends_on.empty()) {
                ss << "    Depends on: ";
                for (size_t j = 0; j < node.depends_on.size(); ++j) {
                    ss << node.depends_on[j];
                    if (j < node.depends_on.size() - 1) ss << ", ";
                }
                ss << "\n";
            }

            if (i < order.size() - 1) {
                ss << "    |\n";
                ss << "    v\n";
            }
        }

        return ss.str();
    }
};

} // namespace ode
