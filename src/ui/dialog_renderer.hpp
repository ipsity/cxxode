#pragma once

#include <string>
#include <vector>

namespace ode {
namespace ui {

/**
 * Dialog 渲染辅助工具
 * 吸收 Pi 的对话框设计理念
 */
class dialog_renderer {
public:
    // 渲染带边框的对话框
    static std::vector<std::string> render_box(
        const std::string& title,
        const std::vector<std::string>& content,
        int width = 60) {

        std::vector<std::string> result;

        // 顶部边框
        result.push_back("╔" + std::string(width - 2, '═') + "╗");

        // 标题（如果有）
        if (!title.empty()) {
            std::string title_line = "║ " + title;
            // 填充到指定宽度
            while (title_line.size() < width - 1) {
                title_line += " ";
            }
            title_line += "║";
            result.push_back(title_line);
            result.push_back("╟" + std::string(width - 2, '─') + "╢");
        }

        // 内容
        for (const auto& line : content) {
            std::string content_line = "║ " + line;
            while (content_line.size() < width - 1) {
                content_line += " ";
            }
            content_line += "║";
            result.push_back(content_line);
        }

        // 底部边框
        result.push_back("╚" + std::string(width - 2, '═') + "╝");

        return result;
    }

    // 渲染选择项（带高亮）
    static std::string render_choice(
        const std::string& text,
        const std::string& detail,
        bool selected,
        int index = -1) {

        std::string result;

        // 选中标记
        if (selected) {
            result = "▶ ";  // 或 "→ "
        } else {
            result = "  ";
        }

        // 索引（如果有）
        if (index >= 0) {
            result += "[" + std::to_string(index + 1) + "] ";
        }

        // 文本
        result += text;

        // 详情（如果有）
        if (!detail.empty()) {
            result += " - " + detail;
        }

        return result;
    }

    // 渲染确认对话框
    static std::vector<std::string> render_confirm(
        const std::string& title,
        const std::string& message,
        const std::vector<std::string>& buttons = {"Enter", "Esc"}) {

        std::vector<std::string> content;
        content.push_back(message);
        content.push_back("");  // 空行

        // 按钮行
        std::string button_line;
        for (size_t i = 0; i < buttons.size(); ++i) {
            if (i > 0) button_line += "  ";
            button_line += "[" + buttons[i] + "]";
        }
        content.push_back(button_line);

        return render_box(title, content, 50);
    }
};

} // namespace ui
} // namespace ode
