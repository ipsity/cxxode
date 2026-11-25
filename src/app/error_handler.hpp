#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace ode {

/**
 * 错误处理器
 * Week 2 Day 9: UX 优化 - 友好的错误消息和恢复建议
 *
 * 功能：
 * - 分析错误类型和原因
 * - 生成友好的错误消息
 * - 提供恢复建议
 * - 自动识别常见错误模式
 */
class error_handler {
public:
    // 错误类型分类
    enum class error_type {
        file_not_found,       // 文件不存在
        permission_denied,    // 权限被拒绝
        command_failed,       // 命令执行失败
        syntax_error,         // 语法错误
        network_error,        // 网络错误
        timeout,              // 超时
        resource_limit,       // 资源限制
        invalid_input,        // 无效输入
        tool_not_found,       // 工具不存在
        unknown               // 未知错误
    };

    // 错误信息
    struct error_info {
        error_type type = error_type::unknown;
        std::string original_message;     // 原始错误消息
        std::string friendly_message;     // 友好的错误消息
        std::string context;              // 错误上下文（如文件路径、命令等）
        std::vector<std::string> details; // 详细信息
    };

    // 恢复建议
    struct recovery_suggestion {
        std::string action;         // 建议的操作
        std::string description;    // 操作描述
        float confidence = 0.0f;    // 建议置信度 0.0-1.0
    };

    error_handler() = default;

    // 分析错误消息并分类
    error_info analyze(std::string_view error_message, std::string_view tool_name = "",
                      std::string_view context = "") const;

    // 提供恢复建议
    std::vector<recovery_suggestion> suggest_recovery(const error_info& info) const;

    // 格式化为友好的错误显示
    std::string format_friendly(const error_info& info,
                               bool include_suggestions = true) const;

private:
    // 错误类型检测
    error_type detect_type(std::string_view error_message) const;

    // 提取上下文信息（文件路径、命令等）
    std::string extract_context(std::string_view error_message) const;

    // 生成友好消息
    std::string make_friendly_message(error_type type, std::string_view context) const;

    // 针对特定错误类型的恢复建议
    std::vector<recovery_suggestion> suggest_for_file_not_found(std::string_view context) const;
    std::vector<recovery_suggestion> suggest_for_permission_denied(std::string_view context) const;
    std::vector<recovery_suggestion> suggest_for_command_failed(std::string_view context) const;
    std::vector<recovery_suggestion> suggest_for_syntax_error(std::string_view context) const;
};

} // namespace ode
