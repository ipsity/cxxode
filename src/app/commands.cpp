#include "app/interactive.hpp"
#include "ext/commands.hpp"
#include "ext/discovery.hpp"
#include "ext/lua/host.hpp"
#include "provider/models.hpp"
#include "ui/theme.hpp"

#include <iostream>

namespace ode {

using ui::paint;
using ui::tone;

namespace {

constexpr const char* help_text =
    "**快捷键参考**\n"
    "\n"
    "**编辑**\n"
    "- `Ctrl+A` 移到行首  `Ctrl+E` 移到行尾\n"
    "- `Ctrl+U` 删除到行首  `Ctrl+K` 删除到行尾\n"
    "- `Ctrl+W` 删除上一个单词\n"
    "- `Ctrl+D` 删除字符（空行退出）\n"
    "- `Backspace` / `Del` 删除字符\n"
    "- `Ctrl+←` / `Ctrl+→` 单词级移动\n"
    "\n"
    "**导航与历史**\n"
    "- `↑` `↓` 浏览命令历史或多行移动\n"
    "- `Page Up` / `Page Down` 或鼠标滚轮翻阅对话，翻到底部后继续跟随新输出\n"
    "- `Home` / `End` 行首/行尾\n"
    "- `←` / `→` 字符移动\n"
    "\n"
    "**输入与提交**\n"
    "- `Enter` 发送；模型工作时发送的内容会插进下一步\n"
    "- `Shift+Enter` 或行尾 `\\` 换行\n"
    "- `Tab` 接受补全建议\n"
    "\n"
    "**控制**\n"
    "- `Esc` 中断当前轮；空闲时清空草稿\n"
    "- `Ctrl+C` 中断或清空，连按两次退出\n"
    "- `Ctrl+L` 清屏\n"
    "- `Ctrl+O` 展开最近一次工具的完整输出\n"
    "- `Ctrl+T` 切换后续思考的显示方式（正文或字数）\n"
    "- `Shift+Tab` 切换权限档：plan → default → yolo\n"
    "- 需要确认时：`Enter` 允许，`Esc` 拒绝，`a` 总是允许\n"
    "\n"
    "**🚀 快捷工作流**（新）\n"
    "- `/ref <file>` 快速查看并编辑 (Read→Edit→Write)\n"
    "- `/search <pattern>` 搜索并查看 (Grep→Read) [别名: /s]\n"
    "- `/fix [file]` 智能检查并修复问题\n"
    "- `/test [pattern]` 运行测试 [别名: /t]\n"
    "- `/build` 构建项目 [别名: /b]\n"
    "- `/run [args]` 运行程序 [别名: /r]\n"
    "- `/commit [msg]` 智能提交 [别名: /c]\n"
    "\n"
    "**命令**\n"
    "- `/help` 显示此帮助  `/keys` 同 /help\n"
    "- `/compact [重点]` 压缩上下文  `/model [名称]` 切换模型\n"
    "- `/mode [档位]` 切换权限档  `/theme` 切换主题\n"
    "- `/resume [序号]` 恢复会话  `/undo` 撤销上一轮改动\n"
    "- `/reload` 重新加载 Lua 扩展\n"
    "- `/login [provider]` 登录或更新密钥  `/logout [provider]` 清除保存的密钥\n"
    "- `/clear` 新会话  `/cost` 查看用量  `/exit` 退出";

std::pair<std::string, std::string> split(const std::string& line) {
    const auto space = line.find_first_of(" \t\r\n");
    if (space == std::string::npos) return {line, {}};
    const auto start = line.find_first_not_of(" \t\r\n", space);
    return {line.substr(0, space), start == std::string::npos ? std::string{} : line.substr(start)};
}

} // namespace

bool interactive_ui::command(const std::string& line) {
    const auto [name, arg] = split(line);
    if (name == "/login") {
        login_command(arg);
        return true;
    }
    if (name == "/logout") {
        logout_command(arg);
        return true;
    }
    section("user");
    // Commands in accent_bold for visibility
    screen_.commit(paint(tone::accent, "› ") + paint(tone::accent_bold, line));
    auto& core = *app_.core;

    if (name == "/exit" || name == "/quit") return false;
    if (name == "/help" || name == "/keys" || name == "/shortcuts") {
        std::string text = help_text;
        if (app_.lua) {
            const auto keys = app_.lua->keys();
            if (!keys.empty()) {
                text += "\n\n**Lua 键位**\n";
                for (const auto& key : keys) text += "- `" + key + "`\n";
            }
        }
        print_markdown(text);
    }
    // 🆕 快捷工作流命令 - 减少重复操作
    else if (name == "/ref") {
        // /ref <file> - 快速 Read-Edit-Write 工作流
        if (arg.empty()) {
            note("用法: /ref <file>  快速查看并编辑文件", tone::error);
        } else {
            note("工作流: Read → Edit → Write " + arg, tone::dim);
            start_turn("Read " + arg + " then suggest edits");
        }
    }
    else if (name == "/search" || name == "/s") {
        // /search <pattern> - 快速 Grep-Read 工作流
        if (arg.empty()) {
            note("用法: /search <pattern>  搜索并查看结果", tone::error);
        } else {
            note("工作流: Grep → Read 匹配的文件", tone::dim);
            start_turn("Grep for " + arg + " then read the matching files");
        }
    }
    else if (name == "/fix") {
        // /fix [file] - 智能修复工作流
        if (arg.empty()) {
            note("工作流: 分析最近错误并修复", tone::dim);
            start_turn("Analyze the recent errors and fix them");
        } else {
            note("工作流: 检查并修复 " + arg, tone::dim);
            start_turn("Check " + arg + " for issues and fix them");
        }
    }
    else if (name == "/test" || name == "/t") {
        // /test [pattern] - 快速运行测试
        if (arg.empty()) {
            note("工作流: 运行所有测试", tone::dim);
            start_turn("Run all tests and show results");
        } else {
            note("工作流: 运行匹配 " + arg + " 的测试", tone::dim);
            start_turn("Run tests matching " + arg + " and show results");
        }
    }
    else if (name == "/build" || name == "/b") {
        // /build - 快速构建项目
        note("工作流: 构建项目", tone::dim);
        start_turn("Build the project using the available build system");
    }
    else if (name == "/run" || name == "/r") {
        // /run [args] - 运行程序
        if (arg.empty()) {
            note("工作流: 运行程序", tone::dim);
            start_turn("Run the main program");
        } else {
            note("工作流: 运行程序带参数: " + arg, tone::dim);
            start_turn("Run the program with arguments: " + arg);
        }
    }
    else if (name == "/commit" || name == "/c") {
        // /commit [message] - 智能提交
        if (arg.empty()) {
            note("工作流: 分析更改并提交", tone::dim);
            start_turn("Review changes and create an appropriate commit");
        } else {
            note("工作流: 提交更改: " + arg, tone::dim);
            start_turn("Commit changes with message: " + arg);
        }
    }
    else if (name == "/lint" || name == "/l") {
        // /lint [file] - 代码检查
        if (arg.empty()) {
            note("工作流: 检查所有代码", tone::dim);
            start_turn("Run linter on all files and show issues");
        } else {
            note("工作流: 检查 " + arg, tone::dim);
            start_turn("Run linter on " + arg + " and suggest fixes");
        }
    }
    else if (name == "/format" || name == "/fmt") {
        // /format [file] - 代码格式化
        if (arg.empty()) {
            note("工作流: 格式化所有代码", tone::dim);
            start_turn("Format all code files according to style guide");
        } else {
            note("工作流: 格式化 " + arg, tone::dim);
            start_turn("Format " + arg + " according to style guide");
        }
    }
    else if (name == "/doc" || name == "/d") {
        // /doc [file] - 生成文档
        if (arg.empty()) {
            note("工作流: 生成项目文档", tone::dim);
            start_turn("Generate or update project documentation");
        } else {
            note("工作流: 为 " + arg + " 生成文档", tone::dim);
            start_turn("Generate or update documentation for " + arg);
        }
    }
    else if (name == "/goal") {
        // 🆕 /goal - Goal 管理
        if (arg.empty()) {
            // 显示当前 goal 或清除
            if (app_.goal_mgr.has_active_goal()) {
                auto status = app_.goal_mgr.format_status();
                note(status, tone::dim);
                note("输入 /goal clear 清除目标", tone::dim);
            } else {
                note("当前没有活跃的 Goal", tone::dim);
                note("用法: /goal <描述>  设置工作目标", tone::dim);
                note("示例: /goal 完成登录功能的实现和测试", tone::dim);
            }
        } else if (arg == "clear") {
            // 清除 goal
            app_.goal_mgr.clear_goal();
            note("✅ Goal 已清除", tone::dim);
        } else {
            // 设置新 goal
            app_.goal_mgr.set_goal(arg);
            note("🎯 Goal 已设置: " + arg, tone::dim);
            note("AI 将持续工作直到完成此目标", tone::dim);
        }
    }
    else if (name == "/clear") {
        core.messages.clear();
        core.total = {};
        core.context_tokens = 0;
        usage_ = {};
        app_.open_session(std::nullopt);
        screen_.clear_screen();
        note("新会话 " + app_.session_id);
    } else if (name == "/reload") {
        start_job([this](agent_events& events, cancel_token& cancel) {
            for (const auto& warning : app_.reload_lua(cancel)) events.notice(warning);
            if (cancel.cancelled()) return turn_result{stop_reason::cancelled, "Cancelled."};
            events.notice("已重新读取 Lua 配置");
            return turn_result{};
        }, false, false);
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
        if (arg.empty()) {
            // Show interactive model selector
            const auto models = list_models(app_.provider_cfg);
            if (models.empty()) {
                note("无可用模型或 provider 不支持列表", tone::error);
            } else {
                // Build selection items
                std::vector<std::string> items;
                std::vector<std::string> details;
                std::size_t current_index = 0;

                for (std::size_t i = 0; i < models.size(); ++i) {
                    items.push_back(models[i].id);

                    std::string detail;
                    if (!models[i].description.empty()) {
                        detail = models[i].description;
                    }
                    if (models[i].context_window > 0) {
                        if (!detail.empty()) detail += " ";
                        detail += "(" + std::to_string(models[i].context_window / 1000) + "k)";
                    }
                    // Set current model as default selection
                    if (models[i].id == core.model) {
                        current_index = i;
                    }
                    details.push_back(detail);
                }

                // Show selection UI
                selection_ = {
                    items,
                    details,
                    current_index,
                    "Select a model from " + app_.config.provider + ":",
                    [this, models](std::size_t index) {
                        app_.set_model(models[index].id);
                        note("✓ Switched to model: " + app_.core->model);
                    },
                    []() { /* Cancel - do nothing */ }
                };
            }
        } else {
            // Direct selection by number or name
            try {
                const auto num = std::stoul(arg);
                const auto models = list_models(app_.provider_cfg);
                if (num > 0 && num <= models.size()) {
                    app_.set_model(models[num - 1].id);
                    note("✓ Switched to model: " + core.model);
                } else {
                    note("Invalid model number", tone::error);
                }
            } catch (...) {
                app_.set_model(arg);
                note("✓ Switched to model: " + core.model);
            }
        }
    } else if (name == "/mode") {
        if (auto m = parse_mode(arg)) app_.permissions.set_mode(*m);
        else if (arg.empty()) app_.permissions.set_mode(next_mode(app_.permissions.mode()));
        else note("档位为 plan、default 或 yolo", tone::error);
        note("权限档 " + std::string(mode_name(app_.permissions.mode())));
    } else if (name == "/theme") {
        if (arg.empty()) {
            // Show interactive theme selector with customize option
            auto themes = available_themes();
            std::vector<std::string> items;
            std::vector<std::string> details;

            // Add all themes
            for (const auto& [name, theme] : themes) {
                items.push_back(name);
                details.push_back("User: " + theme.user_message.ansi_fg() + "■■■\033[0m  "
                                + "Assistant: " + theme.assistant_message.ansi_fg() + "■■■\033[0m  "
                                + "Accent: " + theme.accent.ansi_fg() + "■■■\033[0m");
            }

            // Add customize option
            items.push_back("customize");
            details.push_back("Create or edit custom theme");

            selection_ = {
                items,
                details,
                0,
                "Select a theme:",
                [this, themes](std::size_t index) {
                    std::vector<std::string> names;
                    for (const auto& [name, _] : themes) names.push_back(name);
                    names.push_back("customize");

                    const auto& selected = names[index];

                    if (selected == "customize") {
                        // Show customize menu
                        show_customize_menu();
                    } else {
                        // Apply theme and save selection
                        app_.current_theme = themes.at(selected);
                        save_theme_selection(selected);  // Save for next restart
                        note("✓ Theme applied: " + selected, tone::accent);
                        note("Theme '" + selected + "' is now active!");
                        note("Colors updated for tools and UI elements");
                        note("Theme will be remembered on restart", tone::dim);
                    }
                },
                []() { /* Cancel */ }
            };
        } else {
            note("Use /theme to open theme selector", tone::dim);
        }
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
                show_session_history();
            }
        }
    } else if (app_.lua && std::ranges::find(app_.lua->commands(), name.substr(1)) != app_.lua->commands().end()) {
        start_job([this, name = name.substr(1), arg](agent_events& events, cancel_token& cancel) {
            const auto out = app_.core->run_local([&](tool_context& ctx) {
                return app_.lua->run_command(name, arg, ctx);
            }, events, cancel);
            if (cancel.cancelled()) return turn_result{stop_reason::cancelled, "Cancelled."};
            if (out.is_error) return turn_result{stop_reason::error, out.text};
            if (!out.text.empty()) events.notice(out.text);
            return turn_result{};
        }, false, false);
    } else {
        note("未知命令 " + name + "，/help 查看全部", tone::error);
    }
    return true;
}

void interactive_ui::login_command(std::string_view arg) {
    if (arg.empty()) {
        selection_state providers;
        providers.title = "选择登录的 provider";
        providers.items.push_back(app_.config.provider);
        providers.details.push_back("当前 provider");
        for (const auto& [name, entry] : app_.config.providers) {
            if (name == app_.config.provider) continue;
            providers.items.push_back(name);
        }
        providers.on_select = [this, names = providers.items](std::size_t index) {
            login_ = login_state{names[index], {}};
        };
        selection_ = std::move(providers);
        return;
    }

    const auto [name, key] = split(std::string(arg));
    if (!app_.config.providers.contains(name)) {
        note("未知 provider，输入 /login 查看可选项", tone::error);
        return;
    }
    if (key.find_first_of("\r\n") != std::string::npos) {
        note("API 密钥应为单行", tone::error);
        return;
    }
    login_ = login_state{name, key};
    if (!key.empty()) save_login();
}

void interactive_ui::save_login() {
    if (login_->key.empty()) {
        note("请输入 API 密钥", tone::error);
        return;
    }

    auto [name, key] = std::move(*login_);
    login_.reset();
    if (!save_credential(name, key)) {
        note("保存 Windows 凭据失败，请重新输入 /login", tone::error);
        return;
    }

    note("已保存 " + name + " 的 Windows 凭据", tone::accent);
    provider_config resolved;
    if (!resolve_provider(app_.config, name, resolved) && resolved.api_key != key) {
        note(name + " 仍优先使用配置文件或环境变量中的密钥");
    }
    if (name == app_.config.provider) app_.refresh_credentials();
}

void interactive_ui::logout_command(std::string_view arg) {
    if (arg.find_first_of(" \t\r\n") != std::string_view::npos) {
        note("用法 /logout [provider]", tone::error);
        return;
    }
    const auto name = arg.empty() ? app_.config.provider : std::string(arg);
    if (!delete_credential(name)) {
        note("清除 Windows 凭据失败", tone::error);
        return;
    }

    if (name == app_.config.provider) {
        app_.provider_cfg.api_key.clear();
        app_.provider_cfg.bearer = false;
        app_.credential_error = "当前会话已退出，输入 /login 重新登录";
    }
    note("已清除 " + name + " 的 Windows 凭据", tone::accent);
    provider_config resolved;
    if (!resolve_provider(app_.config, name, resolved) && !resolved.api_key.empty()) {
        note(name + " 的外部密钥会在重新登录或启动时加载");
    }
}

bool interactive_ui::user_command(const std::string& line) {
    const auto [name, arg] = split(line);
    const auto cmd_name = name.substr(1);  // Remove leading /

    for (const auto& cmd : app_.commands) {
        if (cmd.name == cmd_name) {
            const auto content = read_file(cmd.path);
            if (!content) {
                note("无法加载命令 " + cmd_name, tone::error);
                return true;
            }

            auto [meta, body] = parse_frontmatter(*content);
            const auto ws = app_.root.generic_u8string();
            start_turn(expand_command(body, arg, std::string(ws.begin(), ws.end())),
                       frontmatter_list(meta["allowed-tools"], true));
            return true;
        }
    }

    return false;  // Not a user command
}

} // namespace ode
