#include "ui/welcome.hpp"
#include "base/files.hpp"
#include "ui/render.hpp"

#include <fstream>
#include <filesystem>

namespace ode::ui {

namespace fs = std::filesystem;

std::vector<std::string> welcome_screen(int columns) {
    std::vector<std::string> lines;

    const int width = std::min(columns - 4, 70);
    const std::string border_top = "╭─ 欢迎使用 ode " + std::string(width - 15, '-') + "╮";
    const std::string border_bottom = "╰" + std::string(width, '-') + "╯";
    const auto pad = [width](const std::string& text) {
        const int padding = width - text::width(text);
        return "│ " + text + std::string(padding > 0 ? padding : 0, ' ') + " │";
    };

    lines.push_back(border_top);
    lines.push_back(pad(""));
    lines.push_back(pad(paint(tone::bold, "ode") + " 是一个终端 coding agent，可以："));
    lines.push_back(pad(""));
    lines.push_back(pad("  " + paint(tone::accent, "•") + " 读写文件、运行命令、搜索代码"));
    lines.push_back(pad("  " + paint(tone::accent, "•") + " 使用 Skills、MCP 工具、自定义插件"));
    lines.push_back(pad("  " + paint(tone::accent, "•") + " 实时显示费用，三档权限控制"));
    lines.push_back(pad(""));
    lines.push_back(pad(paint(tone::bold, "快速开始：")));
    lines.push_back(pad("  " + paint(tone::code, "ode \"帮我分析 main.cpp\"")));
    lines.push_back(pad("  " + paint(tone::code, "ode --model opus \"code review\"")));
    lines.push_back(pad(""));
    lines.push_back(pad(paint(tone::bold, "快捷键：")));
    lines.push_back(pad("  " + paint(tone::accent, "/help") + "       查看所有命令"));
    lines.push_back(pad("  " + paint(tone::accent, "@") + "           文件补全"));
    lines.push_back(pad("  " + paint(tone::accent, "Shift+Tab") + "  切换权限模式"));
    lines.push_back(pad(""));
    lines.push_back(pad(paint(tone::dim, "输入 /help 查看完整文档，或直接开始对话")));
    lines.push_back(pad(""));
    lines.push_back(border_bottom);
    lines.push_back("");

    return lines;
}

bool should_show_welcome() {
    return !fs::exists(ode_home() / "welcome_shown");
}

void mark_welcome_shown() {
    std::ofstream(ode_home() / "welcome_shown");
}

} // namespace ode::ui
