#include "app/tool_suggestion.hpp"
#include <algorithm>
#include <cctype>
#include <regex>

namespace ode {
namespace {

// 检查文本是否包含关键词（不区分大小写）
bool contains_keyword(std::string_view text, const std::vector<std::string>& keywords) {
    std::string lower_text;
    lower_text.reserve(text.size());
    for (char c : text) lower_text += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    for (const auto& keyword : keywords) {
        std::string lower_keyword;
        lower_keyword.reserve(keyword.size());
        for (char c : keyword) lower_keyword += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

        if (lower_text.find(lower_keyword) != std::string::npos) {
            return true;
        }
    }
    return false;
}

} // namespace

bool tool_suggestion_engine::contains_file_path(std::string_view text) const {
    // 检测文件路径模式：包含 . 和 / 或 \，或常见扩展名
    std::regex file_pattern(R"(\b\w+\.(cpp|hpp|h|c|py|js|ts|java|go|rs|md|txt|json|yaml|yml|toml|xml|html|css)\b)",
                           std::regex_constants::icase);
    return std::regex_search(text.begin(), text.end(), file_pattern);
}

bool tool_suggestion_engine::contains_directory_path(std::string_view text) const {
    // 检测目录路径：src/, docs/, build/, 等
    std::regex dir_pattern(R"(\b\w+/|\bsrc\b|\bdocs\b|\bbuild\b|\btest\b|\blib\b)");
    return std::regex_search(text.begin(), text.end(), dir_pattern);
}

bool tool_suggestion_engine::contains_search_intent(std::string_view text) const {
    static const std::vector<std::string> keywords = {
        "搜索", "查找", "找到", "search", "find", "grep", "look for", "locate"
    };
    return contains_keyword(text, keywords);
}

bool tool_suggestion_engine::contains_execute_intent(std::string_view text) const {
    static const std::vector<std::string> keywords = {
        "运行", "执行", "构建", "编译", "测试",
        "run", "execute", "build", "compile", "test", "make", "cmake"
    };
    return contains_keyword(text, keywords);
}

bool tool_suggestion_engine::contains_debug_intent(std::string_view text) const {
    static const std::vector<std::string> keywords = {
        "错误", "bug", "调试", "修复", "问题",
        "error", "debug", "fix", "issue", "problem", "fail"
    };
    return contains_keyword(text, keywords);
}

bool tool_suggestion_engine::contains_edit_intent(std::string_view text) const {
    static const std::vector<std::string> keywords = {
        "修改", "编辑", "改", "更新", "替换",
        "edit", "modify", "change", "update", "replace"
    };
    return contains_keyword(text, keywords);
}

std::vector<std::string> tool_suggestion_engine::extract_file_paths(std::string_view text) const {
    std::vector<std::string> paths;
    std::regex file_pattern(R"(\b[\w/\\.-]+\.(cpp|hpp|h|c|py|js|ts|java|go|rs|md|txt|json|yaml|yml|toml|xml|html|css)\b)",
                           std::regex_constants::icase);

    auto begin = std::cregex_iterator(text.begin(), text.end(), file_pattern);
    auto end = std::cregex_iterator();

    for (auto it = begin; it != end; ++it) {
        paths.push_back(it->str());
    }
    return paths;
}

std::vector<tool_suggestion_engine::suggestion> tool_suggestion_engine::analyze(std::string_view user_input) const {
    std::vector<suggestion> suggestions;

    // 1. 检测文件路径 → 推荐 Read
    if (contains_file_path(user_input)) {
        auto files = extract_file_paths(user_input);
        if (!files.empty()) {
            suggestions.push_back({
                "Read",
                "检测到文件路径: " + files[0],
                "读取文件内容",
                0.9f
            });

            // 如果有编辑意图，也推荐 Edit
            if (contains_edit_intent(user_input)) {
                suggestions.push_back({
                    "Edit",
                    "检测到编辑意图",
                    "修改文件内容",
                    0.85f
                });
            }
        }
    }

    // 2. 检测搜索意图 → 推荐 Grep/Glob
    if (contains_search_intent(user_input)) {
        suggestions.push_back({
            "Grep",
            "检测到搜索意图",
            "搜索文件内容",
            0.85f
        });
        suggestions.push_back({
            "Glob",
            "检测到搜索意图",
            "查找文件路径",
            0.80f
        });
    }

    // 3. 检测执行意图 → 推荐 Bash
    if (contains_execute_intent(user_input)) {
        suggestions.push_back({
            "Bash",
            "检测到执行意图",
            "运行命令",
            0.90f
        });
    }

    // 4. 检测目录路径 → 推荐 Glob
    if (contains_directory_path(user_input)) {
        suggestions.push_back({
            "Glob",
            "检测到目录路径",
            "列出匹配的文件",
            0.75f
        });
    }

    // 5. 检测调试意图 → 推荐 Read + Grep
    if (contains_debug_intent(user_input)) {
        suggestions.push_back({
            "Grep",
            "检测到调试需求",
            "搜索错误信息",
            0.80f
        });
        suggestions.push_back({
            "Read",
            "检测到调试需求",
            "查看日志文件",
            0.75f
        });
    }

    // 去重（相同工具名只保留最高置信度的）
    std::vector<suggestion> unique_suggestions;
    for (const auto& sugg : suggestions) {
        auto it = std::find_if(unique_suggestions.begin(), unique_suggestions.end(),
                              [&](const suggestion& s) { return s.tool_name == sugg.tool_name; });
        if (it == unique_suggestions.end()) {
            unique_suggestions.push_back(sugg);
        } else if (sugg.confidence > it->confidence) {
            *it = sugg;  // 替换为更高置信度的
        }
    }

    // 按置信度排序
    std::sort(unique_suggestions.begin(), unique_suggestions.end());

    // 最多返回 3 个建议
    if (unique_suggestions.size() > 3) {
        unique_suggestions.resize(3);
    }

    return unique_suggestions;
}

void tool_suggestion_engine::record_tool_use(std::string_view tool_name) {
    history_.push_back(std::string(tool_name));
    if (history_.size() > max_history_size) {
        history_.pop_front();
    }
}

std::vector<tool_suggestion_engine::suggestion> tool_suggestion_engine::suggest_next(std::string_view last_tool) const {
    std::vector<suggestion> suggestions;

    // 常见工具使用模式
    if (last_tool == "Read") {
        suggestions.push_back({"Edit", "Read 之后通常需要编辑", "修改文件", 0.70f});
        suggestions.push_back({"Grep", "深入搜索相关内容", "搜索模式", 0.60f});
    } else if (last_tool == "Edit") {
        suggestions.push_back({"Write", "保存修改", "写入文件", 0.80f});
        suggestions.push_back({"Read", "验证修改结果", "重新读取", 0.65f});
    } else if (last_tool == "Write") {
        suggestions.push_back({"Bash", "测试修改", "运行测试", 0.75f});
        suggestions.push_back({"Read", "确认写入结果", "读取验证", 0.60f});
    } else if (last_tool == "Grep") {
        suggestions.push_back({"Read", "查看匹配的文件", "读取详情", 0.75f});
        suggestions.push_back({"Edit", "修改匹配结果", "编辑文件", 0.65f});
    } else if (last_tool == "Glob") {
        suggestions.push_back({"Read", "查看找到的文件", "读取内容", 0.75f});
        suggestions.push_back({"Bash", "批量处理文件", "执行操作", 0.70f});
    } else if (last_tool == "Bash") {
        suggestions.push_back({"Read", "查看输出或日志", "读取结果", 0.70f});
        suggestions.push_back({"Grep", "搜索输出内容", "过滤结果", 0.60f});
    }

    return suggestions;
}

std::vector<std::string> tool_suggestion_engine::recent_tools(size_t count) const {
    std::vector<std::string> result;
    const size_t start = history_.size() > count ? history_.size() - count : 0;
    for (size_t i = start; i < history_.size(); ++i) {
        result.push_back(history_[i]);
    }
    return result;
}

} // namespace ode
