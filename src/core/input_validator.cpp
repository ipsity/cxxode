#include "core/input_validator.hpp"
#include <stack>

namespace ode {

input_validator::validation_result input_validator::check_quotes(std::string_view input) const {
    char in_quote = '\0';  // 当前在哪种引号内（'\0' = 不在引号内）
    bool escaped = false;
    size_t quote_start = 0;

    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];

        if (escaped) {
            escaped = false;
            continue;
        }

        if (c == '\\') {
            escaped = true;
            continue;
        }

        if (c == '"' || c == '\'') {
            if (in_quote == '\0') {
                // 开始引号
                in_quote = c;
                quote_start = i;
            } else if (in_quote == c) {
                // 结束引号
                in_quote = '\0';
            }
        }
    }

    if (in_quote != '\0') {
        return {
            issue_type::unclosed_quote,
            std::string("未闭合的") + (in_quote == '"' ? "双" : "单") + "引号",
            quote_start,
            false,
            in_quote
        };
    }

    return {issue_type::none, "", 0, false};
}

input_validator::validation_result input_validator::check_brackets(std::string_view input) const {
    std::stack<std::pair<char, size_t>> stack;  // 括号和位置
    char in_quote = '\0';
    bool escaped = false;

    for (size_t i = 0; i < input.size(); ++i) {
        char c = input[i];

        // 处理转义
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }

        // 跟踪引号状态（括号在引号内不计数）
        if (c == '"' || c == '\'') {
            if (in_quote == '\0') {
                in_quote = c;
            } else if (in_quote == c) {
                in_quote = '\0';
            }
            continue;
        }

        // 如果在引号内，跳过括号检查
        if (in_quote != '\0') continue;

        // 检查括号
        if (c == '(' || c == '[' || c == '{') {
            stack.push({c, i});
        } else if (c == ')' || c == ']' || c == '}') {
            if (stack.empty()) {
                return {
                    issue_type::mismatched_bracket,
                    std::string("多余的闭合括号: ") + c,
                    i,
                    false,
                    c
                };
            }

            char open = stack.top().first;
            char expected_close = (open == '(') ? ')' : (open == '[') ? ']' : '}';

            if (c != expected_close) {
                return {
                    issue_type::mismatched_bracket,
                    std::string("括号不匹配: 期望 ") + expected_close + " 但遇到 " + c,
                    i,
                    false,
                    expected_close
                };
            }

            stack.pop();
        }
    }

    if (!stack.empty()) {
        auto [open_bracket, pos] = stack.top();
        char expected = (open_bracket == '(') ? ')' : (open_bracket == '[') ? ']' : '}';
        return {
            issue_type::unclosed_bracket,
            std::string("未闭合的括号: ") + open_bracket,
            pos,
            false,
            expected
        };
    }

    return {issue_type::none, "", 0, false};
}

input_validator::validation_result input_validator::check_syntax(std::string_view input) const {
    // 检查末尾反斜杠（续行标记）
    if (!input.empty() && input.back() == '\\') {
        // 检查是否是转义的反斜杠
        size_t backslash_count = 0;
        for (size_t i = input.size(); i > 0 && input[i - 1] == '\\'; --i) {
            backslash_count++;
        }

        // 奇数个反斜杠表示末尾有未转义的反斜杠
        if (backslash_count % 2 == 1) {
            return {
                issue_type::trailing_backslash,
                "末尾有反斜杠（续行）",
                input.size() - 1,
                true,  // 这是警告，不是错误
                '\0'
            };
        }
    }

    // 检查空引号对
    bool in_quote = false;
    char quote_char = '\0';
    size_t quote_start = 0;
    bool escaped = false;

    for (size_t i = 0; i < input.size(); ++i) {
        if (escaped) {
            escaped = false;
            continue;
        }

        if (input[i] == '\\') {
            escaped = true;
            continue;
        }

        if (input[i] == '"' || input[i] == '\'') {
            if (!in_quote) {
                in_quote = true;
                quote_char = input[i];
                quote_start = i;
            } else if (input[i] == quote_char) {
                // 检查是否是空引号
                if (i == quote_start + 1) {
                    return {
                        issue_type::empty_quotes,
                        "空引号",
                        quote_start,
                        true,  // 警告
                        '\0'
                    };
                }
                in_quote = false;
            }
        }
    }

    return {issue_type::none, "", 0, false};
}

input_validator::validation_result input_validator::validate(std::string_view input) const {
    // 按优先级检查

    // 1. 检查引号（最高优先级）
    auto quote_result = check_quotes(input);
    if (quote_result.type != issue_type::none) {
        return quote_result;
    }

    // 2. 检查括号
    auto bracket_result = check_brackets(input);
    if (bracket_result.type != issue_type::none) {
        return bracket_result;
    }

    // 3. 检查其他语法问题
    auto syntax_result = check_syntax(input);
    if (syntax_result.type != issue_type::none) {
        return syntax_result;
    }

    return {issue_type::none, "", 0, false};
}

bool input_validator::has_unclosed_quotes(std::string_view input) const {
    return check_quotes(input).type == issue_type::unclosed_quote;
}

bool input_validator::has_unclosed_brackets(std::string_view input) const {
    auto result = check_brackets(input);
    return result.type == issue_type::unclosed_bracket ||
           result.type == issue_type::mismatched_bracket;
}

} // namespace ode
