#include "app/app.hpp"
#include "app/builtin_commands.hpp"
#include "ext/commands.hpp"
#include "app/rpc.hpp"
#include "core/version.hpp"
#include "ext/agents.hpp"
#include "ext/hooks.hpp"
#include "ext/lua/host.hpp"
#include "ext/mcp.hpp"
#include "ext/skills.hpp"
#include "tools/tool.hpp"
#include "ui/render.hpp"
#include "ui/term.hpp"

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
    "  ode login [provider]      store an API key in Windows Credential Manager\n"
    "  ode logout [provider]     remove the saved API key\n"
    "  ode models                list available models from current provider\n"
    "\n"
    "  --model NAME              model id or alias (opus, sonnet, haiku, fable)\n"
    "  --provider NAME           anthropic, openai, or one from settings.json\n"
    "  --permission MODE         plan | default | yolo\n"
    "  -C DIR                    workspace directory\n"
    "  --no-color                plain output (also NO_COLOR=1)\n"
    "  -v, --version\n"
    "\n"
    "Settings: ~/.ode/settings.json, .claude/settings.json, .ode/settings.json\n";

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

    lua_host lua;
    app a;
    a.reload_lua = [&lua](cancel_token& cancel) { return lua.reload({ode_home() / "init.lua"}, cancel); };
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
            if (!delete_credential(name)) return fail("could not remove the saved key");
            ui::write_stream(true, "Cleared Windows Credential Manager entry ode/" + name + "\n");
            return 0;
        }
        if (!a.config.providers.contains(name)) return fail("unknown provider " + name);
        ui::write_stream(true, "API key for " + name + ": ");
        const auto key = read_secret();
        ui::write_stream(true, "\n");
        if (key.empty()) return fail("no key entered");
        if (!save_credential(name, key)) return fail("could not save the key");
        ui::write_stream(true, "Saved to Windows Credential Manager as ode/" + name + "\n");
        return 0;
    }

    // Initialize provider config early for commands that need it
    if (auto error = resolve_provider(a.config, a.config.provider, a.provider_cfg)) return fail(*error);

    if (list_models) {
        return cmd_models(a, "");
    }

    if (!mode_text.empty()) {
        const auto m = parse_mode(mode_text);
        if (!m) return fail("--permission takes plan, default or yolo");
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

    // Assemble the tool list from each ext module. The task tool gets a copy of the
    // tools registered before it, so a subagent runs everything except task itself.
    auto tools = builtin_tools();
    const auto append = [&tools](std::vector<tool> more) {
        tools.insert(tools.end(), std::make_move_iterator(more.begin()), std::make_move_iterator(more.end()));
    };
    append(make_skill_tools(a.skills));
    append(make_mcp_tools(a.mcp_clients));
    auto hooks = make_agent_hooks(a.config.hooks, [&a] { return hook_session{a.session_id, a.log.path(), a.root}; });
    append(make_task_tools(a.agents, *a.model_provider, tools, a.permissions, [&a] { return a.core->model; }, hooks));

    a.core = std::make_unique<agent>(
        *a.model_provider,
        std::move(tools),
        a.permissions,
        workspace,
        std::move(hooks)
    );

    const auto& entry = a.config.providers.at(a.config.provider);
    a.set_model(!model.empty() ? model : !a.config.model.empty() ? a.config.model : entry.default_model);
    a.core->system = build_system_prompt(workspace, shell_name(), load_memory(workspace));
    a.core->system += skills_system_prompt(a.skills);

    // Piped stdin is part of the prompt, as in `git diff | ode -p "review this"`.
    if (!ui::stdin_is_terminal()) {
        std::string piped((std::istreambuf_iterator<char>(std::cin)), std::istreambuf_iterator<char>());
        if (!piped.empty()) prompt = prompt.empty() ? piped : prompt + "\n\n" + piped;
        print = true;
    }

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
        return run_interactive(a, prompt);
    } catch (const std::exception& e) {
        return fail(e.what());
    }
}
