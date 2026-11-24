#include "app/app.hpp"
#include "core/version.hpp"
#include "skills/skill.hpp"
#include "tools/tool.hpp"
#include "ui/render.hpp"
#include "ui/term.hpp"

#include <cstdlib>
#include <iostream>
#include <iterator>
#include <string>
#include <vector>

#ifdef _WIN32
#include "platform/text.hpp"
#include <shellapi.h>
#endif

using namespace lay;

namespace {

constexpr const char* usage_text =
    "lay — a terminal coding agent\n"
    "\n"
    "  lay [prompt]              interactive session in the current directory\n"
    "  lay -p \"prompt\"           print mode: answer to stdout, tool calls to stderr\n"
    "  lay -c                    continue the most recent session here\n"
    "  lay login [provider]      store an API key in Windows Credential Manager\n"
    "\n"
    "  --model NAME              model id\n"
    "  --provider NAME           anthropic, openai, or one from settings.json\n"
    "  --permission MODE         plan | default | yolo\n"
    "  -C DIR                    workspace directory\n"
    "  --no-color                plain output (also NO_COLOR=1)\n"
    "  -v, --version\n"
    "\n"
    "Settings: ~/.lay/settings.json, .claude/settings.json, .lay/settings.json\n";

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
    ui::write_stream(true, "lay: " + message + "\n");
    return 2;
}

std::string read_secret() {
#ifdef _WIN32
    const HANDLE in = GetStdHandle(STD_INPUT_HANDLE);
    DWORD mode = 0;
    const bool console = GetConsoleMode(in, &mode);
    if (console) SetConsoleMode(in, mode & ~static_cast<DWORD>(ENABLE_ECHO_INPUT));
    std::wstring line;
    wchar_t buf[512];
    DWORD got = 0;
    if (console && ReadConsoleW(in, buf, 512, &got, nullptr)) line.assign(buf, got);
    if (console) SetConsoleMode(in, mode);
    std::string key = narrow(line);
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
    std::string prompt, model, provider_name, mode_text;
    fs::path workspace = fs::current_path();
    bool print = false, cont = false, login = false;
    if (const char* nc = std::getenv("NO_COLOR"); nc && *nc) ui::color_enabled() = false;

    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        const auto value = [&]() -> std::string {
            if (i + 1 >= args.size()) throw std::invalid_argument(a + " needs a value");
            return args[++i];
        };
        try {
            if (a == "-v" || a == "--version") {
                ui::write_stream(false, "lay " + std::string(version()) + "\n");
                return 0;
            } else if (a == "-h" || a == "--help") {
                ui::write_stream(false, usage_text);
                return 0;
            } else if (a == "-p" || a == "--print") {
                print = true;
            } else if (a == "-c" || a == "--continue") {
                cont = true;
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
            } else if (!a.empty() && a.front() == '-' && a.size() > 1) {
                return fail("unknown option " + a + ". See lay --help.");
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

    app a;
    a.root = workspace;
    a.config = load_settings(workspace);
    for (const auto& w : a.config.warnings) ui::write_stream(true, "lay: " + w + "\n");
    if (!provider_name.empty()) a.config.provider = provider_name;

    if (login) {
        const auto name = prompt.empty() ? a.config.provider : prompt;
        ui::write_stream(true, "API key for " + name + ": ");
        const auto key = read_secret();
        ui::write_stream(true, "\n");
        if (key.empty()) return fail("no key entered");
        if (!save_credential(name, key)) return fail("could not save the key");
        ui::write_stream(true, "Saved to Windows Credential Manager as lay/" + name + "\n");
        return 0;
    }
    if (!mode_text.empty()) {
        const auto m = parse_mode(mode_text);
        if (!m) return fail("--permission takes plan, default or yolo");
        a.config.permissions.set_mode(*m);
    }

    if (auto error = resolve_provider(a.config, a.config.provider, a.provider_cfg)) return fail(*error);
    a.model_provider = make_provider(a.provider_cfg);
    a.skills = load_skills(workspace);
    a.core = std::make_unique<agent>(*a.model_provider, make_tools(a.skills), a.config.permissions, workspace);
    const auto& entry = a.config.providers.at(a.config.provider);
    a.set_model(!model.empty() ? model : !a.config.model.empty() ? a.config.model : entry.default_model);
    a.core->system = build_system_prompt(workspace, shell_name(), load_memory(workspace));
    a.core->system += skills_system_prompt(a.skills);

    // Piped stdin is part of the prompt, as in `git diff | lay -p "review this"`.
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

    if (print) {
        if (prompt.empty()) return fail("-p needs a prompt");
        if (mode_text.empty()) a.config.permissions.set_mode(permission_mode::plan);
        return run_print(a, prompt);
    }
    if (!ui::terminal::interactive()) return fail("not a terminal; use lay -p \"...\"");
    try {
        return run_interactive(a, prompt);
    } catch (const std::exception& e) {
        return fail(e.what());
    }
}
