#pragma once

#include "innovation/tool_chain.hpp"
#include <map>
#include <string>
#include <optional>

namespace ode {

/**
 * 工具链执行器
 * Week 4 Day 23: 创新功能 - 工具链执行
 *
 * 功能：
 * - 执行工具链
 * - 参数替换
 * - 结果传递
 */

struct execution_result {
    bool success = false;
    std::string output;
    std::string error;
    std::map<std::string, std::string> outputs;  // 各节点输出
};

class chain_executor {
public:
    chain_executor() = default;

    // 执行工具链
    execution_result execute(const tool_chain& chain) {
        execution_result result;
        result.success = true;

        // 验证链
        if (!chain.validate()) {
            result.success = false;
            result.error = "Invalid chain: cycle detected";
            return result;
        }

        // 获取执行顺序
        auto order = chain.execution_order();

        // 按顺序执行每个节点
        for (const auto& node_id : order) {
            const auto& node = chain.nodes().at(node_id);

            // 准备参数（替换变量）
            auto params = prepare_params(node.params, result.outputs);

            // 执行工具（实际实现中会调用真实的工具）
            auto output = execute_tool(node.tool_name, params);

            if (!output) {
                result.success = false;
                result.error = "Tool failed: " + node.tool_name;
                return result;
            }

            // 保存输出
            if (!node.output_key.empty()) {
                result.outputs[node.output_key] = *output;
            }

            result.output += *output + "\n";
        }

        return result;
    }

    // 设置工具执行回调
    using tool_executor_fn = std::function<std::optional<std::string>(
        const std::string& tool_name,
        const std::map<std::string, std::string>& params)>;

    void set_executor(tool_executor_fn executor) {
        executor_ = std::move(executor);
    }

private:
    // 准备参数（替换 $variable）
    std::map<std::string, std::string> prepare_params(
        const std::map<std::string, std::string>& params,
        const std::map<std::string, std::string>& outputs) {

        std::map<std::string, std::string> prepared;

        for (const auto& [key, value] : params) {
            if (value.starts_with("$")) {
                // 变量替换
                auto var_name = value.substr(1);
                if (outputs.count(var_name)) {
                    prepared[key] = outputs.at(var_name);
                } else {
                    prepared[key] = value;  // 保持原值
                }
            } else {
                prepared[key] = value;
            }
        }

        return prepared;
    }

    // 执行工具（占位符实现）
    std::optional<std::string> execute_tool(
        const std::string& tool_name,
        const std::map<std::string, std::string>& params) {

        if (executor_) {
            return executor_(tool_name, params);
        }

        // 默认实现（模拟）
        return "Executed: " + tool_name;
    }

    tool_executor_fn executor_;
};

/**
 * 预定义工作流执行器
 */
class workflow_runner {
public:
    // 执行 Read-Edit-Write 工作流
    static execution_result run_read_edit_write(
        const std::string& file,
        chain_executor& executor) {

        auto chain = chain_builder::read_edit_write(file);
        return executor.execute(chain);
    }

    // 执行 Search-Read 工作流
    static execution_result run_search_read(
        const std::string& pattern,
        chain_executor& executor) {

        auto chain = chain_builder::search_and_read(pattern);
        return executor.execute(chain);
    }
};

} // namespace ode
