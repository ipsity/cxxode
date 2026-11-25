#include "app/error_handler.hpp"
#include <algorithm>
#include <cctype>
#include <regex>

namespace ode {
namespace {

// 检查文本是否包含关键词（不区分大小写）
bool contains_ignore_case(std::string_view text, std::string_view keyword) {
    std::string lower_text;
    lower_text.reserve(text.size());
    for (char c : text) lower_text += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    std::string lower_keyword;
    lower_keyword.reserve(keyword.size());
    for (char c : keyword) lower_keyword += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    return lower_text.find(lower_keyword) != std::string::npos;
}

} // namespace

error_handler::error_type error_handler::detect_type(std::string_view error_message) const {
    // File not found patterns
    if (contains_ignore_case(error_message, "no such file") ||
        contains_ignore_case(error_message, "file not found") ||
        contains_ignore_case(error_message, "cannot find") ||
        contains_ignore_case(error_message, "does not exist")) {
        return error_type::file_not_found;
    }

    // Permission denied patterns
    if (contains_ignore_case(error_message, "permission denied") ||
        contains_ignore_case(error_message, "access denied") ||
        contains_ignore_case(error_message, "denied by") ||
        contains_ignore_case(error_message, "已拒绝")) {
        return error_type::permission_denied;
    }

    // Command failed patterns
    if (contains_ignore_case(error_message, "command not found") ||
        contains_ignore_case(error_message, "command failed") ||
        contains_ignore_case(error_message, "exit code") ||
        contains_ignore_case(error_message, "failed with")) {
        return error_type::command_failed;
    }

    // Syntax error patterns
    if (contains_ignore_case(error_message, "syntax error") ||
        contains_ignore_case(error_message, "parse error") ||
        contains_ignore_case(error_message, "invalid syntax") ||
        contains_ignore_case(error_message, "unexpected")) {
        return error_type::syntax_error;
    }

    // Network error patterns
    if (contains_ignore_case(error_message, "network") ||
        contains_ignore_case(error_message, "connection") ||
        contains_ignore_case(error_message, "timeout") ||
        contains_ignore_case(error_message, "unreachable")) {
        return error_type::network_error;
    }

    // Resource limit patterns
    if (contains_ignore_case(error_message, "out of memory") ||
        contains_ignore_case(error_message, "resource limit") ||
        contains_ignore_case(error_message, "disk full") ||
        contains_ignore_case(error_message, "too large")) {
        return error_type::resource_limit;
    }

    // Tool not found
    if (contains_ignore_case(error_message, "unknown tool") ||
        contains_ignore_case(error_message, "未知工具")) {
        return error_type::tool_not_found;
    }

    // Invalid input
    if (contains_ignore_case(error_message, "invalid") ||
        contains_ignore_case(error_message, "参数错误") ||
        contains_ignore_case(error_message, "must be")) {
        return error_type::invalid_input;
    }

    return error_type::unknown;
}

std::string error_handler::extract_context(std::string_view error_message) const {
    // Try to extract file path
    std::regex file_pattern(R"(['"]([^'"]+\.[a-zA-Z0-9]+)['"]|([/\\]?[\w/\\.-]+\.[a-zA-Z0-9]+))");
    std::cmatch match;
    if (std::regex_search(error_message.begin(), error_message.end(), match, file_pattern)) {
        return match[1].matched ? match[1].str() : match[2].str();
    }

    // Try to extract command name (after "command " or at start of line)
    std::regex cmd_pattern(R"(command\s+['"]?(\w+)['"]?|^['"]?(\w+)['"]?\s+not found)");
    if (std::regex_search(error_message.begin(), error_message.end(), match, cmd_pattern)) {
        return match[1].matched ? match[1].str() : match[2].str();
    }

    return {};
}

std::string error_handler::make_friendly_message(error_type type, std::string_view context) const {
    switch (type) {
        case error_type::file_not_found:
            if (!context.empty()) {
                return "找不到文件: " + std::string(context);
            }
            return "找不到指定的文件";

        case error_type::permission_denied:
            return "操作被权限系统拒绝";

        case error_type::command_failed:
            if (!context.empty()) {
                return "命令执行失败: " + std::string(context);
            }
            return "命令执行失败";

        case error_type::syntax_error:
            return "语法错误或输入格式不正确";

        case error_type::network_error:
            return "网络连接出现问题";

        case error_type::timeout:
            return "操作超时";

        case error_type::resource_limit:
            return "资源限制：内存不足或文件过大";

        case error_type::invalid_input:
            return "输入参数无效或格式不正确";

        case error_type::tool_not_found:
            if (!context.empty()) {
                return "未知的工具: " + std::string(context);
            }
            return "未知的工具";

        case error_type::unknown:
        default:
            return "操作失败";
    }
}

error_handler::error_info error_handler::analyze(std::string_view error_message,
                                                 std::string_view tool_name,
                                                 std::string_view explicit_context) const {
    error_info info;
    info.original_message = std::string(error_message);
    info.type = detect_type(error_message);
    info.context = explicit_context.empty() ? extract_context(error_message) : std::string(explicit_context);
    info.friendly_message = make_friendly_message(info.type, info.context);

    // Add tool name to context if provided
    if (!tool_name.empty()) {
        info.details.push_back("工具: " + std::string(tool_name));
    }

    return info;
}

std::vector<error_handler::recovery_suggestion> error_handler::suggest_recovery(const error_info& info) const {
    switch (info.type) {
        case error_type::file_not_found:
            return suggest_for_file_not_found(info.context);
        case error_type::permission_denied:
            return suggest_for_permission_denied(info.context);
        case error_type::command_failed:
            return suggest_for_command_failed(info.context);
        case error_type::syntax_error:
            return suggest_for_syntax_error(info.context);
        default:
            return {};
    }
}

std::vector<error_handler::recovery_suggestion>
error_handler::suggest_for_file_not_found(std::string_view context) const {
    std::vector<recovery_suggestion> suggestions;

    suggestions.push_back({
        "检查文件路径是否正确",
        "确认文件名拼写和路径无误",
        0.9f
    });

    if (!context.empty()) {
        suggestions.push_back({
            "使用 Glob 搜索相似文件",
            "用 Glob 工具查找类似名称的文件",
            0.85f
        });
    }

    suggestions.push_back({
        "使用 Bash ls 列出目录内容",
        "查看目录中实际存在的文件",
        0.80f
    });

    return suggestions;
}

std::vector<error_handler::recovery_suggestion>
error_handler::suggest_for_permission_denied(std::string_view context) const {
    (void)context;  // May be used in future for context-specific suggestions
    std::vector<recovery_suggestion> suggestions;

    suggestions.push_back({
        "检查权限模式",
        "当前可能处于 plan 模式（只读），使用 Shift+Tab 切换到 normal 或 yolo 模式",
        0.95f
    });

    suggestions.push_back({
        "查看权限规则",
        "某些操作可能被权限规则阻止，考虑调整 settings.json 中的规则",
        0.75f
    });

    suggestions.push_back({
        "使用 /mode 命令",
        "快速切换权限档位：/mode yolo 允许所有操作",
        0.80f
    });

    return suggestions;
}

std::vector<error_handler::recovery_suggestion>
error_handler::suggest_for_command_failed(std::string_view context) const {
    std::vector<recovery_suggestion> suggestions;

    if (!context.empty()) {
        suggestions.push_back({
            "检查命令是否存在",
            "使用 'which " + std::string(context) + "' 检查命令是否可用",
            0.85f
        });
    }

    suggestions.push_back({
        "查看完整错误输出",
        "使用 Ctrl+O 查看完整的命令输出和错误详情",
        0.90f
    });

    suggestions.push_back({
        "检查命令语法",
        "确认命令参数和选项正确",
        0.75f
    });

    return suggestions;
}

std::vector<error_handler::recovery_suggestion>
error_handler::suggest_for_syntax_error(std::string_view context) const {
    (void)context;  // May be used in future for context-specific suggestions
    std::vector<recovery_suggestion> suggestions;

    suggestions.push_back({
        "检查输入格式",
        "确认输入符合工具的要求格式",
        0.85f
    });

    suggestions.push_back({
        "查看工具文档",
        "使用 /help 查看工具的正确用法",
        0.75f
    });

    return suggestions;
}

std::string error_handler::format_friendly(const error_info& info, bool include_suggestions) const {
    std::string result = "❌ " + info.friendly_message;

    if (!info.context.empty()) {
        result += "\n   " + info.context;
    }

    if (include_suggestions) {
        auto suggestions = suggest_recovery(info);
        if (!suggestions.empty()) {
            result += "\n\n💡 建议:";
            for (size_t i = 0; i < std::min<size_t>(suggestions.size(), 3); ++i) {
                result += "\n   " + std::to_string(i + 1) + ". " + suggestions[i].action;
                if (!suggestions[i].description.empty()) {
                    result += " - " + suggestions[i].description;
                }
            }
        }
    }

    return result;
}

} // namespace ode
