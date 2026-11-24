#include "app/interactive.hpp"

namespace lay {

using ui::paint;
using ui::tone;

namespace {

constexpr const char* help_text =
    "**按键**\n"
    "- `Enter` 发送；模型工作时发送的内容会插进下一步\n"
    "- `Ctrl+J`、`Shift+Enter` 或行尾 `\\` 换行\n"
    "- `Esc` 中断当前轮；空闲时清空草稿\n"
    "- `Ctrl+C` 中断或清空，连按两次退出；`Ctrl+D` 空草稿时退出\n"
    "- `Shift+Tab` 切换权限档：plan 只读 → default → yolo 全放行\n"
    "- `Ctrl+O` 展开最近一次工具的完整输出\n"
    "- `↑` `↓` 输入历史；`Ctrl+L` 清屏\n"
    "- 需要确认时：`Enter` 允许，`Esc` 拒绝，`a` 本会话总是允许\n"
    "\n"
    "**命令**\n"
    "- `/compact [重点]` 压缩上下文  `/model [名称]` 模型  `/mode [档位]` 权限档\n"
    "- `/resume [序号]` 恢复会话  `/undo` 撤销上一轮改动  `/clear` 新会话\n"
    "- `/cost` 用量  `/exit` 退出";

std::pair<std::string, std::string> split(const std::string& line) {
    const auto space = line.find(' ');
    if (space == std::string::npos) return {line, {}};
    auto arg = line.substr(space + 1);
    while (!arg.empty() && arg.front() == ' ') arg.erase(0, 1);
    return {line.substr(0, space), arg};
}

} // namespace

bool interactive_ui::command(const std::string& line) {
    const auto [name, arg] = split(line);
    section("user");
    screen_.commit(paint(tone::accent, "› ") + paint(tone::dim, line));
    auto& core = *app_.core;

    if (name == "/exit" || name == "/quit") return false;
    if (name == "/help") {
        print_markdown(help_text);
    } else if (name == "/clear") {
        core.messages.clear();
        core.total = {};
        core.context_tokens = 0;
        usage_ = {};
        app_.open_session(std::nullopt);
        screen_.clear_screen();
        note("新会话 " + app_.session_id);
    } else if (name == "/compact") {
        if (core.messages.empty()) {
            note("没有可压缩的内容");
        } else {
            start_job([this, focus = arg](agent_events& ev, cancel_token& c) { return app_.core->compact(ev, c, focus); }, true);
        }
    } else if (name == "/undo") {
        const auto restored = core.undo();
        if (restored.empty()) note("没有可撤销的改动");
        for (const auto& p : restored) note("已恢复 " + p);
    } else if (name == "/model") {
        if (arg.empty()) note("当前模型 " + core.model + "。用 /model 名称 切换");
        else {
            app_.set_model(arg);
            note("模型已切换为 " + core.model);
        }
    } else if (name == "/mode") {
        if (auto m = parse_mode(arg)) app_.config.permissions.set_mode(*m);
        else if (arg.empty()) app_.config.permissions.set_mode(next_mode(app_.config.permissions.mode()));
        else note("档位为 plan、default 或 yolo", tone::error);
        note("权限档 " + std::string(mode_name(app_.config.permissions.mode())));
    } else if (name == "/cost") {
        const auto& t = core.total;
        note("输入 " + short_count(t.input) + "  缓存读 " + short_count(t.cache_read) + "  缓存写 " +
             short_count(t.cache_write) + "  输出 " + short_count(t.output) + "  当前上下文 " + short_count(core.context_tokens));
    } else if (name == "/resume") {
        const auto sessions = list_sessions(app_.root);
        if (sessions.empty()) {
            note("这个目录还没有会话");
        } else if (arg.empty()) {
            for (std::size_t i = 0; i < sessions.size() && i < 10; ++i) {
                const auto title = sessions[i].title.empty() ? "(空)" : ui::one_line(sessions[i].title);
                note(std::to_string(i + 1) + "  " + text::keep_head(title, size_.columns - 12), tone::normal);
            }
            note("用 /resume 序号 打开");
        } else {
            const auto index = static_cast<std::size_t>(std::atoi(arg.c_str()));
            if (index < 1 || index > sessions.size()) {
                note("没有这个序号", tone::error);
            } else {
                app_.open_session(sessions[index - 1].path);
                usage_ = {};
                note("已恢复 " + std::to_string(core.messages.size()) + " 条消息：" + ui::one_line(sessions[index - 1].title));
            }
        }
    } else {
        note("未知命令 " + name + "，/help 查看全部", tone::error);
    }
    return true;
}

} // namespace lay
