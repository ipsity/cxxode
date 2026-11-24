#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace ode {

/**
 * 输入验证器
 * Week 2 Day 12: 交互优化 - 输入验证和提示
 *
 * 功能：
 * - 检测未闭合引号
 * - 检测未闭合括号
 * - 检测常见语法错误
 * - 提供即时反馈
 */
class input_validator {
public:
    enum class issue_type {
        unclosed_quote,       // 未闭合引号
        unclosed_bracket,     // 未闭合括号
        mismatched_bracket,   // 括号不匹配
        trailing_backslash,   // 末尾反斜杠（可能是续行）
        empty_quotes,         // 空引号
        none                  // 无问题
    };

    struct validation_result {
        issue_type type = issue_type::none;
        std::string message;        // 友好的提示消息
        size_t position = 0;        // 问题位置
        bool is_warning = false;    // true=警告, false=错误
        char expected_char = '\0';  // 期望的字符（如闭合括号）
    };

    input_validator() = default;

    // 验证输入
    validation_result validate(std::string_view input) const;

    // 检查特定问题
    bool has_unclosed_quotes(std::string_view input) const;
    bool has_unclosed_brackets(std::string_view input) const;

private:
    // 查找未闭合引号
    validation_result check_quotes(std::string_view input) const;

    // 查找未闭合括号
    validation_result check_brackets(std::string_view input) const;

    // 检查其他语法问题
    validation_result check_syntax(std::string_view input) const;
};

} // namespace ode
