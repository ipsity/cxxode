#include "app/app.hpp"
#include "app/builtin_commands.hpp"
#include "ext/commands.hpp"
#include "app/rpc.hpp"
#include "core/version.hpp"
#include "ext/agents.hpp"
#include "ext/discovery.hpp"
#include "ext/hooks.hpp"
#include "ext/lua/host.hpp"
#include "ext/mcp.hpp"
#include "ext/skills.hpp"
#include "tools/tool.hpp"
#include "ui/render.hpp"
#include "ui/term.hpp"

#include <algorithm>
#include <cstdlib>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
#include "base/utf.hpp"
#include <shellapi.h>
#endif

using namespace ode;

namespace {

constexpr const char* usage_text =
    "ode — a terminal coding agent\n"
    "\n"
    "  ode [prompt]              interactive session in the current directory\n"
    "  ode -p \"prompt\"           print mode: answer to stdout, tool calls to stderr\n"
    "  ode -c                    continue the most recent session here\n"
    "  ode --rpc                 RPC mode: JSONL protocol over stdin/stdout\n"
    "  ode login [provider]      save or replace api_key in ~/.ode/settings.json\n"
    "  ode login [provider] --import-windows-credential\n"
    "                            import an old ode/<provider> Windows credential\n"
    "  ode logout [provider]     remove the saved API key\n"
    "  ode models                list available models from current provider\n"
    "\n"
    "  --model NAME              model id or alias (opus, sonnet, haiku, fable)\n"
    "  --provider NAME           anthropic, openai, deepseek, or one from ~/.ode/settings.json\n"
    "  --permission MODE         plan | auto | yolo\n"
    "  -C DIR                    workspace directory\n"
    "  --no-color                plain output (also NO_COLOR=1)\n"
    "  -v, --version\n"
    "\n"
    "Provider settings: ~/.ode/settings.json, .ode/settings.json\n";

std::vector<std::string> utf8_args(int argc, char** argv) {
#ifdef _WIN32
    (void)argc;
    (void)argv;
    int n = 0;
    LPWSTR* wide = CommandLineToArgvW(GetCommandLineW(), &n);
    std::vector<std::string> out;
    for (int i = 1; i < n; ++i) out.push_back(narrow(wide[i]));
    LocalFree(wide);
    return out;
#else
    return std::vector<std::string>(argv + 1, argv + argc);
#endif
}

int fail(const std::string& message) {
    ui::write_stream(true, "ode: " + message + "\n");
    return 2;
}

std::string read_secret() {
#ifdef _WIN32
    const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    const bool console = GetConsoleMode(in, &mode);
    if (console && !SetConsoleMode(in, mode & ~static_cast<DWORD>(ENABLE_ECHO_INPUT))) return {};
    std::string key;
    wchar_t buf[4096];
    DWORD got = 0;
    if (console && ReadConsoleW(in, buf, std::size(buf), &got, nullptr)) key = narrow({buf, got});
    if (console) SetConsoleMode(in, mode);
    else std::getline(std::cin, key);
#else
    std::string key;
    std::getline(std::cin, key);
#endif
    while (!key.empty() && (key.back() == '\n' || key.back() == '\r' || key.back() == ' ')) key.pop_back();
    return key;
}

} // namespace

int main(int argc, char** argv) {
    const auto args = utf8_args(argc, argv);
    // Hidden command the MCP tests launch. It must not touch the console or load settings:
    // both would race with the client that owns its stdin.
    if (!args.empty() && args[0] == "--mcp-test-server") return run_mcp_test_server();

    std::string prompt, model, provider_name, mode_text;
    fs::path workspace = fs::current_path();
    bool print = false, cont = false, login = false, logout = false, rpc_mode = false, list_models = false;
    bool import_windows = false;
    if (const char* nc = std::getenv("NO_COLOR"); nc && *nc) ui::color_enabled() = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= args.size()) throw std::invalid_argument(a + " needs a value");
            return args[++i];
        };
        try {
            if (a == "-v" || a == "--version") {
                ui::write_stream(false, "ode " + std::string(version()) + "\n");
                return 0;
            } else if (a == "-h" || a == "--help") {
                ui::write_stream(false, usage_text);
                return 0;
            } else if (a == "-p" || a == "--print") {
                print = true;
            } else if (a == "-c" || a == "--continue") {
                cont = true;
            } else if (a == "--rpc") {
                rpc_mode = true;
            } else if (a == "--model" || a == "-m") {
                model = value();
            } else if (a == "--provider") {
                provider_name = value();
            } else if (a == "--permission" || a == "--permission-mode") {
                mode_text = value();
            } else if (a == "-C") {
                workspace = fs::path(std::u8string(reinterpret_cast<const char8_t*>(value().c_str())));
            } else if (a == "--no-color") {
                ui::color_enabled() = false;
            } else if (a == "login" && i == 0) {
                login = true;
            } else if (a == "--import-windows-credential" && login) {
                import_windows = true;
            } else if (a == "logout" && i == 0) {
                logout = true;
            } else if (a == "models" && i == 0) {
                list_models = true;
            } else if (!a.empty() && a.front() == '-' && a.size() > 1) {
                return fail("unknown option " + a + ". See ode --help.");
            } else {
                prompt += (prompt.empty() ? "" : " ") + a;
            }
        } catch (const std::exception& e) {
            return fail(e.what());
        }
    }

    std::error_code ec;
    workspace = fs::weakly_canonical(fs::absolute(workspace), ec);
    if (!fs::is_directory(workspace, ec)) return fail("not a directory: " + workspace.string());

    lua_host lua([](std::string_view text) {
        ui::write_stream(true, "lua: " + std::string(text) + "\n");
    });
    app a;
    a.lua = &lua;
    a.root = workspace;
    a.config = load_settings(workspace);
    a.permissions.allow = a.config.permission_allow;
    a.permissions.deny = a.config.permission_deny;
    if (auto m = parse_mode(a.config.permission_mode)) a.permissions.set_mode(*m);
    for (const auto& w : a.config.warnings) ui::write_stream(true, "ode: " + w + "\n");
    if (!provider_name.empty()) a.config.provider = provider_name;

    if (login || logout) {
        const auto name = prompt.empty() ? a.config.provider : prompt;
        if (name.find_first_of(" \t\r\n") != std::string::npos) return fail("expected a single provider name");
        if (logout) {
            if (!delete_credential(a.config, name)) return fail("could not remove api_key from user .ode/settings.json");
            ui::write_stream(true, "Removed saved key for " + name + "\n");
            return 0;
        }
        const auto entry = a.config.providers.find(name);
        if (entry == a.config.providers.end()) return fail("unknown provider " + name);
        if (entry->second.auth == credential_mode::none) return fail(name + " is configured for unauthenticated access");
        if (entry->second.base_url.empty()) return fail("configure base_url for " + name + " in ~/.ode/settings.json");
        if (import_windows) {
            if (!import_windows_credential(a.config, name)) return fail("could not import Windows credential ode/" + name);
            ui::write_stream(true, "Imported credential for " + entry->second.base_url + "\n");
        } else {
            ui::write_stream(true, "API key for " + name + ": ");
            const auto key = read_secret();
            ui::write_stream(true, "\n");
            if (!save_credential(a.config, name, key)) return fail("could not save api_key to user .ode/settings.json");
        }
        if (!save_provider_selection(name, a.config.user_settings_path)) {
            return fail("key saved, but could not persist the provider selection");
        }
        ui::write_stream(true, "Saved API key and selected " + name + "\n");
        return 0;
    }

    // Piped stdin is part of the prompt, as in `git diff | ode -p \"review this\"`.
    if (!list_models && !rpc_mode && !ui::stdin_is_terminal()) {
        std::string piped((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
        if (!piped.empty()) prompt = prompt.empty() ? piped : prompt + "\n\n" + piped;
        print = true;
    }
    if (!a.config.providers.contains(a.config.provider)) return fail("Unknown provider: " + a.config.provider);
    a.credential_error = resolve_provider(a.config, a.config.provider, a.provider_cfg);
    if (a.credential_error && (list_models || rpc_mode || print)) return fail(*a.credential_error);

    if (list_models) {
        return cmd_models(a, "");
    }

    if (!mode_text.empty()) {
        const auto m = parse_mode(mode_text);
        if (!m) return fail("--permission takes plan, auto or yolo");
        a.permissions.set_mode(*m);
    }

    a.model_provider = make_provider(a.provider_cfg);
    a.skills = load_skills(workspace);
    a.commands = load_commands(workspace);

    // Load saved theme selection
    const auto saved_theme_name = load_theme_selection();
    a.current_theme = load_theme(saved_theme_name);

    // Initialize MCP servers
    if (!a.config.mcp_servers.is_null() && a.config.mcp_servers.is_object()) {
        for (const auto& [name, config] : a.config.mcp_servers.items()) {
            if (!config.is_object()) continue;
            mcp_client::server_config cfg;
            cfg.name = name;
            cfg.command = config.value("command", "");
            if (cfg.command.empty()) continue;
            if (config.contains("args") && config["args"].is_array()) {
                for (const auto& arg : config["args"]) {
                    if (arg.is_string()) cfg.args.push_back(arg);
                }
            }
            if (config.contains("env") && config["env"].is_object()) {
                for (const auto& [k, v] : config["env"].items()) {
                    if (v.is_string()) cfg.env[k] = v;
                }
            }
            cfg.working_dir = config.value("cwd", "");
            auto client = std::make_shared<mcp_client>(cfg);
            if (client->initialize()) {
                a.mcp_clients.push_back(std::move(client));
            } else {
                ui::write_stream(true, "ode: failed to initialize MCP server: " + name + "\n");
            }
        }
    }

    // Load subagent configs
    a.agents = load_agents(workspace);

    const auto assemble_extensions = [&](std::vector<std::string>& warnings) {
        auto tools = builtin_tools();
        const auto append = [&tools](std::vector<tool> more) {
            tools.insert(tools.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
        };
        append(make_skill_tools(a.skills));
        append(make_mcp_tools(a.mcp_clients));
        auto lua_tools = lua.tools();
        std::erase_if(lua_tools, [&](const tool& t) {
            const bool conflict = t.name == "Task" ||
                std::ranges::find(tools, t.name, &tool::name) != tools.end();
            if (conflict) warnings.push_back("Lua tool name is reserved: " + t.name);
            return conflict;
        });
        append(std::move(lua_tools));
        script_hooks script;
        if (lua.has_hooks()) {
            script = [&lua](hook_type type, const json& input, cancel_token& cancel) {
                return lua.run_hooks(type, input, cancel, parse_hook_output);
            };
        }
        auto hooks = make_agent_hooks(a.config.hooks, [&a] {
            return hook_session{a.session_id, a.log.path(), a.root};
        }, std::move(script));
        // A subagent receives every registered tool except Task itself.
        append(make_task_tools(a.agents, *a.model_provider, tools, a.permissions, [&a] { return a.core->model; }, hooks));
        a.core->set_extensions(std::move(tools), std::move(hooks));
    };
    a.core = std::make_unique<agent>(*a.model_provider, std::vector<tool>{}, a.permissions, workspace);
    a.reload_lua = [&](cancel_token& cancel) {
        std::vector<lua_script> scripts{{ode_home() / "init.lua", {}, {}}};
        auto plugins = load_plugins();
        std::ranges::sort(plugins, {}, &plugin_info::path);
        for (const auto& plugin : plugins) {
            scripts.push_back({plugin.path / "ode" / "init.lua", plugin.name, plugin.version});
        }
        auto warnings = lua.reload_scripts(std::move(scripts), cancel);
        assemble_extensions(warnings);
        return warnings;
    };

    a.set_model(model.empty() ? configured_model(a.config, a.config.provider) : model);
    a.core->system = build_system_prompt(workspace, shell_name(), load_memory(workspace));
    a.core->system += skills_system_prompt(a.skills);

    std::optional<fs::path> resume;
    if (cont) {
        const auto sessions = list_sessions(workspace);
        if (sessions.empty()) return fail("no earlier session in this directory");
        resume = sessions.front().path;
    }
    a.open_session(resume);

    cancel_token startup;
    for (const auto& warning : a.reload_lua(startup)) ui::write_stream(true, "ode: " + warning + "\n");

    if (rpc_mode) {
        // RPC mode: JSONL protocol over stdin/stdout
        cancel_token cancel;
        rpc_server server{*a.core, cancel};
        server.run();
        return 0;
    }

    if (print) {
        if (prompt.empty()) return fail("-p needs a prompt");
        // Headless runs cannot ask, and default mode writes inside the workspace without asking,
        // so stay read-only unless the user chose a mode on the command line or in settings.
        if (mode_text.empty() && !parse_mode(a.config.permission_mode)) a.permissions.set_mode(permission_mode::plan);
        return run_print(a, prompt);
    }
    if (!ui::terminal::interactive()) return fail("not a terminal; use ode -p \"...\"");
    try {
        if (mode_text.empty()) {
            if (const auto saved = read_file(ode_home() / "permission_mode.txt")) {
                if (auto mode = parse_mode(*saved)) a.permissions.set_mode(*mode);
            }
        }
        return run_interactive(a, prompt);
    } catch (const std::exception& e) {
        return fail(e.what());
    }
}
