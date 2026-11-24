#include "app/interactive.hpp"
#include "commands/command.hpp"
#include "provider/models.hpp"
#include "theme/theme.hpp"

#include <iostream>

namespace ode {

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
    // Commands in accent_bold for visibility
    screen_.commit(paint(tone::accent, "› ") + paint(tone::accent_bold, line));
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
        if (auto m = parse_mode(arg)) app_.config.permissions.set_mode(*m);
        else if (arg.empty()) app_.config.permissions.set_mode(next_mode(app_.config.permissions.mode()));
        else note("档位为 plan、default 或 yolo", tone::error);
        note("权限档 " + std::string(mode_name(app_.config.permissions.mode())));
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
    } else if (name == "/login") {
        if (arg.empty()) {
            // Show interactive provider selector
            const std::vector<std::pair<std::string, std::string>> providers = {
                {"anthropic", "Claude models from Anthropic"},
                {"openai", "GPT models from OpenAI"},
                {"groq", "Fast inference with Groq"},
                {"deepseek", "DeepSeek models"},
                {"together", "Together.ai models"},
                {"custom", "Configure custom provider"}
            };

            std::vector<std::string> items;
            std::vector<std::string> details;
            for (const auto& [name, desc] : providers) {
                items.push_back(name);
                // Mark current provider
                std::string detail = desc;
                if (name == app_.config.provider) {
                    detail += " (current)";
                }
                details.push_back(detail);
            }

            selection_ = {
                items,
                details,
                0,
                "Select a provider (current: " + app_.config.provider + "):",
                [this](std::size_t index) {
                    const std::vector<std::string> provider_names = {
                        "anthropic", "openai", "groq", "deepseek", "together", "custom"
                    };
                    const auto provider = provider_names[index];

                    if (provider == app_.config.provider) {
                        note("Already using " + provider, tone::accent);
                        note("To update API key: /login " + provider + " <new-key>");
                    } else if (provider == "custom") {
                        note("Custom provider setup:", tone::accent);
                        note("1. Edit ~/.ode/settings.json");
                        note("2. Add your provider configuration");
                        note("3. Use /login <provider-name> <api-key>");
                    } else {
                        note("Selected: " + provider, tone::accent);
                        note("To set API key:");
                        note("  /login " + provider + " <your-api-key>");
                        note("Then restart with: --provider " + provider);
                    }
                },
                []() { /* Cancel */ }
            };
        } else {
            // Direct login with provider and key
            const auto space = arg.find(' ');
            if (space == std::string::npos) {
                note("Missing API key. Usage: /login <provider> <api-key>", tone::error);
            } else {
                const auto provider = arg.substr(0, space);
                const auto api_key = arg.substr(space + 1);

                if (save_credential(provider, api_key)) {
                    note("✓ API key saved for " + provider);
                    if (provider == app_.config.provider) {
                        note("Key updated for current provider");
                    } else {
                        note("Restart with --provider " + provider + " to use it");
                    }
                } else {
                    note("Failed to save credential", tone::error);
                }
            }
        }
    } else if (name == "/logout") {
        note("Current provider: " + app_.config.provider);
        note("To switch provider, restart with --provider <name>");
        note("Credentials remain in Windows Credential Manager");
        note("Use 'ode login <provider>' to update keys");
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
            std::map<std::string, std::string> vars;
            vars["ARGUMENTS"] = arg;
            const auto ws = app_.root.generic_u8string();
            vars["WORKSPACE"] = std::string(ws.begin(), ws.end());

            const auto expanded = expand_variables(body, vars);
            start_turn(expanded);
            return true;
        }
    }

    return false;  // Not a user command
}

} // namespace ode
