#include "config/config.hpp"
#include "core/session.hpp"
#include "hooks/hooks.hpp"
#include "tools/tool.hpp"

#include <cstdlib>
#include <memory>

#ifdef _WIN32
#include "platform/text.hpp"
#include <wincred.h>
#endif

namespace ode {
namespace {

std::string env(const std::string& name) {
    if (name.empty()) return {};
    const char* v = std::getenv(name.c_str());
    return v ? v : "";
}

fs::path user_home() { return ode_home().parent_path(); }

void merge(settings& s, const json& j) {
    if (j.contains("provider") && j["provider"].is_string()) s.provider = j["provider"];
    if (j.contains("model") && j["model"].is_string()) s.model = j["model"];
    if (j.contains("env") && j["env"].is_object()) {
        for (const auto& [k, v] : j["env"].items()) {
            if (!v.is_string()) continue;
#ifdef _WIN32
            _putenv_s(k.c_str(), v.get<std::string>().c_str());
#else
            setenv(k.c_str(), v.get<std::string>().c_str(), 1);
#endif
        }
    }
    if (j.contains("providers") && j["providers"].is_object()) {
        for (const auto& [name, p] : j["providers"].items()) {
            auto& e = s.providers[name];
            e.name = name;
            if (auto k = parse_protocol(p.value("protocol", ""))) e.kind = *k;
            e.base_url = p.value("base_url", e.base_url);
            e.api_key_env = p.value("api_key_env", e.api_key_env);
            e.default_model = p.value("model", e.default_model);
        }
    }
    if (j.contains("permissions") && j["permissions"].is_object()) {
        const auto& p = j["permissions"];
        for (const auto& key : {"allow", "deny"}) {
            if (!p.contains(key) || !p[key].is_array()) continue;
            auto& list = std::string_view(key) == "allow" ? s.permissions.allow : s.permissions.deny;
            for (const auto& r : p[key]) if (r.is_string()) list.push_back(r);
        }
        if (auto m = parse_mode(p.value("defaultMode", ""))) s.permissions.set_mode(*m);
    }
    if (j.contains("hooks") && j["hooks"].is_object()) {
        const auto& hooks = j["hooks"];
        for (const auto& [type_name, hook_list] : hooks.items()) {
            if (!hook_list.is_array()) continue;
            hook_type type;
            if (type_name == "PreToolUse") type = hook_type::pre_tool_use;
            else if (type_name == "PostToolUse") type = hook_type::post_tool_use;
            else if (type_name == "UserPromptSubmit") type = hook_type::user_prompt_submit;
            else if (type_name == "Stop") type = hook_type::stop;
            else continue;  // Unknown hook type

            for (const auto& hook : hook_list) {
                if (!hook.is_object()) continue;
                hook_config cfg;
                cfg.type = type;
                cfg.command = hook.value("command", "");
                if (cfg.command.empty()) continue;
                if (hook.contains("args") && hook["args"].is_array()) {
                    for (const auto& arg : hook["args"]) {
                        if (arg.is_string()) cfg.args.push_back(arg);
                    }
                }
                cfg.working_dir = hook.value("cwd", "");
                s.hooks.push_back(cfg);
            }
        }
    }
    if (j.contains("mcpServers") && j["mcpServers"].is_object()) {
        // Merge MCP server configs (later definitions override earlier)
        if (s.mcp_servers.is_null()) {
            s.mcp_servers = json::object();
        }
        for (const auto& [name, config] : j["mcpServers"].items()) {
            s.mcp_servers[name] = config;
        }
    }
}

void merge_file(settings& s, const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return;
    const auto text = read_file(path);
    auto j = json::parse(text.value_or(""), nullptr, false, true);
    if (j.is_discarded() || !j.is_object()) {
        s.warnings.push_back("Ignored invalid JSON in " + path.string());
        return;
    }
    merge(s, j);
    s.loaded_files.push_back(path);
}

} // namespace

std::optional<protocol> parse_protocol(std::string_view text) {
    if (text == "anthropic" || text == "messages") return protocol::anthropic;
    if (text == "openai-chat" || text == "chat-completions" || text == "chat") return protocol::openai_chat;
    if (text == "openai-responses" || text == "responses" || text == "openai") return protocol::openai_responses;
    return std::nullopt;
}

std::string resolve_model_alias(const std::string& model) {
    struct alias { const char* name; const char* variable; const char* fallback; };
    static constexpr alias aliases[] = {
        {"opus", "ANTHROPIC_DEFAULT_OPUS_MODEL", "claude-opus-4-8"},
        {"sonnet", "ANTHROPIC_DEFAULT_SONNET_MODEL", "claude-sonnet-4-6"},
        {"haiku", "ANTHROPIC_DEFAULT_HAIKU_MODEL", "claude-haiku-4-5-20251001"},
        {"fable", "ANTHROPIC_DEFAULT_FABLE_MODEL", "claude-fable-5"},
    };
    // "opus[1m]" keeps its window suffix after resolution.
    const auto bracket = model.find('[');
    const auto base = model.substr(0, bracket);
    const auto suffix = bracket == std::string::npos ? std::string{} : model.substr(bracket);
    for (const auto& a : aliases) {
        if (base != a.name) continue;
        const auto mapped = env(a.variable);
        return (mapped.empty() ? std::string(a.fallback) : mapped) + suffix;
    }
    return model;
}

settings load_settings(const fs::path& workspace) {
    settings s;
    // Built-in provider configurations
    s.providers["anthropic"] = {"anthropic", protocol::anthropic,
                                env("ANTHROPIC_BASE_URL").empty() ? "https://api.anthropic.com" : env("ANTHROPIC_BASE_URL"),
                                "ANTHROPIC_API_KEY", "ANTHROPIC_AUTH_TOKEN", "claude-sonnet-4-6"};
    s.providers["openai"] = {"openai", protocol::openai_chat,
                             env("OPENAI_BASE_URL").empty() ? "https://api.openai.com/v1" : env("OPENAI_BASE_URL"),
                             "OPENAI_API_KEY", "", "gpt-4o"};

    // OpenAI-compatible providers
    s.providers["groq"] = {"groq", protocol::openai_chat,
                           env("GROQ_BASE_URL").empty() ? "https://api.groq.com/openai/v1" : env("GROQ_BASE_URL"),
                           "GROQ_API_KEY", "", "llama-3.3-70b-versatile"};
    s.providers["together"] = {"together", protocol::openai_chat,
                               env("TOGETHER_BASE_URL").empty() ? "https://api.together.xyz/v1" : env("TOGETHER_BASE_URL"),
                               "TOGETHER_API_KEY", "", "meta-llama/Meta-Llama-3.1-70B-Instruct-Turbo"};
    s.providers["fireworks"] = {"fireworks", protocol::openai_chat,
                                env("FIREWORKS_BASE_URL").empty() ? "https://api.fireworks.ai/inference/v1" : env("FIREWORKS_BASE_URL"),
                                "FIREWORKS_API_KEY", "", "accounts/fireworks/models/llama-v3p1-70b-instruct"};
    s.providers["deepseek"] = {"deepseek", protocol::openai_chat,
                               env("DEEPSEEK_BASE_URL").empty() ? "https://api.deepseek.com/v1" : env("DEEPSEEK_BASE_URL"),
                               "DEEPSEEK_API_KEY", "", "deepseek-chat"};

    // Claude Code's own model variables, so an existing gateway setup works unchanged.
    for (const auto* var : {"ANTHROPIC_DEFAULT_SONNET_MODEL", "ANTHROPIC_MODEL"}) {
        if (!env(var).empty()) s.providers["anthropic"].default_model = env(var);
    }
    const auto home = user_home();
    merge_file(s, home / ".claude" / "settings.json");
    merge_file(s, ode_home() / "settings.json");
    merge_file(s, workspace / ".claude" / "settings.json");
    merge_file(s, workspace / ".claude" / "settings.local.json");
    merge_file(s, workspace / ".ode" / "settings.json");
    if (s.providers.contains(s.provider) == false) {
        s.warnings.push_back("Unknown provider '" + s.provider + "'; using anthropic.");
        s.provider = "anthropic";
    }
    return s;
}

std::optional<std::string> resolve_provider(const settings& s, const std::string& name, provider_config& out) {
    const auto it = s.providers.find(name);
    if (it == s.providers.end()) return "Unknown provider: " + name;
    const auto& e = it->second;
    out.name = e.name;
    out.kind = e.kind;
    out.base_url = e.base_url;
    if (!env(e.api_key_env).empty()) {
        out.api_key = env(e.api_key_env);
        return std::nullopt;
    }
    if (!env(e.auth_token_env).empty()) {
        out.api_key = env(e.auth_token_env);
        out.bearer = true;
        return std::nullopt;
    }
#ifdef _WIN32
    PCREDENTIALW cred = nullptr;
    const auto target = widen("ode/" + name);
    if (CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &cred)) {
        std::unique_ptr<CREDENTIALW, decltype(&CredFree)> owner(cred, CredFree);
        out.api_key.assign(reinterpret_cast<const char*>(cred->CredentialBlob), cred->CredentialBlobSize);
        return std::nullopt;
    }
#endif
    if (e.api_key_env.empty()) return std::nullopt;  // unauthenticated local server
    return "No API key for " + name + ". Set " + e.api_key_env + " or run: ode login " + name;
}

bool save_credential(const std::string& provider, const std::string& key) {
#ifdef _WIN32
    auto target = widen("ode/" + provider);
    CREDENTIALW cred{};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = target.data();
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<char*>(key.data()));
    cred.CredentialBlobSize = static_cast<DWORD>(key.size());
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&cred, 0) != 0;
#else
    (void)provider;
    (void)key;
    return false;
#endif
}

std::vector<std::pair<fs::path, std::string>> load_memory(const fs::path& workspace) {
    std::vector<std::pair<fs::path, std::string>> out;
    const auto add = [&](const fs::path& p) {
        std::error_code ec;
        if (!fs::is_regular_file(p, ec)) return;
        if (auto text = read_file(p); text && !text->empty()) out.emplace_back(p, clip(*text, 40000));
    };
    add(user_home() / ".claude" / "CLAUDE.md");
    add(ode_home() / "ODE.md");
    // Ancestors first so the nearest file has the last word.
    std::vector<fs::path> dirs;
    for (auto d = workspace; ; d = d.parent_path()) {
        dirs.push_back(d);
        if (d == d.parent_path() || fs::exists(d / ".git")) break;
    }
    for (auto it = dirs.rbegin(); it != dirs.rend(); ++it) {
        for (const auto* name : {"CLAUDE.md", "AGENTS.md", "ODE.md", "CLAUDE.local.md"}) add(*it / name);
        add(*it / ".claude" / "CLAUDE.md");
    }
    return out;
}

} // namespace ode
