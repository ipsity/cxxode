#include "config/config.hpp"
#include "base/files.hpp"
#include "base/text.hpp"

#include <cstdlib>
#include <fstream>
#include <memory>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincred.h>
#include "base/utf.hpp"
#endif

namespace ode {
namespace fs = std::filesystem;
namespace {

std::string env(const std::string& name) {
    if (name.empty()) return {};
    const char* v = std::getenv(name.c_str());
    return v ? v : "";
}

void merge(settings& s, const json& j) {
    if (j.contains("provider") && j["provider"].is_string()) s.provider = j["provider"];
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
            if (const auto model = p.find("model"); model != p.end() && model->is_string()) {
                e.default_model = model->get<std::string>();
                s.model_overrides.erase(name);
            }
            e.api_key = p.value("api_key", e.api_key);  // Parse direct API key
        }
    }
    if (j.contains("model") && j["model"].is_string()) {
        const auto provider = j.find("model_provider");
        const auto& owner = provider != j.end() && provider->is_string() ? provider->get_ref<const std::string&>() : s.provider;
        s.model_overrides[owner] = j["model"].get<std::string>();
    }
    if (j.contains("permissions") && j["permissions"].is_object()) {
        const auto& p = j["permissions"];
        for (const auto& key : {"allow", "deny"}) {
            if (!p.contains(key) || !p[key].is_array()) continue;
            auto& list = std::string_view(key) == "allow" ? s.permission_allow : s.permission_deny;
            for (const auto& r : p[key]) if (r.is_string()) list.push_back(r);
        }
        if (p.contains("defaultMode") && p["defaultMode"].is_string()) s.permission_mode = p["defaultMode"];
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

            for (const auto& group : hook_list) {
                if (!group.is_object() || !group.contains("hooks") || !group["hooks"].is_array()) continue;
                const auto matcher = group.contains("matcher") && group["matcher"].is_string()
                    ? group["matcher"].get<std::string>() : std::string{};
                std::optional<std::regex> pattern;
                if ((type == hook_type::pre_tool_use || type == hook_type::post_tool_use) &&
                    !matcher.empty() && matcher != "*") {
                    try {
                        pattern.emplace(matcher);
                    } catch (const std::regex_error&) {
                        s.warnings.push_back("Invalid " + type_name + " hook matcher; using exact match: " + matcher);
                    }
                }
                for (const auto& hook : group["hooks"]) {
                    if (!hook.is_object() || !hook.contains("type") || hook["type"] != "command" ||
                        !hook.contains("command") || !hook["command"].is_string()) continue;
                    auto command = hook["command"].get<std::string>();
                    if (command.empty()) continue;
                    auto timeout = std::chrono::seconds(60);
                    if (hook.contains("timeout") && hook["timeout"].is_number_integer()) {
                        const auto& value = hook["timeout"];
                        constexpr auto max_seconds = std::chrono::duration_cast<std::chrono::seconds>(
                            std::chrono::milliseconds::max()).count();
                        if (value > 0 && value <= max_seconds) timeout = std::chrono::seconds(value.get<std::int64_t>());
                    }
                    s.hooks.push_back({type, matcher, std::move(command), timeout, pattern});
                }
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

// Claude Code keeps MCP servers in .mcp.json, not settings.json. The file is either the
// server map itself or {"mcpServers": {...}}; both shapes appear in the wild.
void merge_mcp_file(settings& s, const fs::path& path) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return;
    const auto text = read_file(path);
    auto j = json::parse(text.value_or(""), nullptr, false, true);
    if (j.is_discarded() || !j.is_object()) {
        s.warnings.push_back("Ignored invalid JSON in " + path.string());
        return;
    }
    if (j.contains("mcpServers")) merge(s, j);
    else merge(s, json{{"mcpServers", j}});
    s.loaded_files.push_back(path);
}

} // namespace

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
                             "OPENAI_API_KEY", "", "gpt-5"};

    s.providers["deepseek"] = {"deepseek", protocol::openai_chat,
                               env("DEEPSEEK_BASE_URL").empty() ? "https://api.deepseek.com/v1" : env("DEEPSEEK_BASE_URL"),
                               "DEEPSEEK_API_KEY", "", "deepseek-flash"};

    // Claude Code's own model variables, so an existing gateway setup works unchanged.
    for (const auto* var : {"ANTHROPIC_DEFAULT_SONNET_MODEL", "ANTHROPIC_MODEL"}) {
        if (!env(var).empty()) s.providers["anthropic"].default_model = env(var);
    }
    const auto home = user_home();
    // Later files override earlier ones. .mcp.json is Claude Code's server list and is
    // read at each of the same scopes, after that scope's settings.json.
    merge_file(s, home / ".claude" / "settings.json");
    merge_mcp_file(s, home / ".claude" / ".mcp.json");
    merge_file(s, ode_home() / "settings.json");
    merge_mcp_file(s, ode_home() / ".mcp.json");
    merge_file(s, workspace / ".claude" / "settings.json");
    merge_file(s, workspace / ".claude" / "settings.local.json");
    merge_mcp_file(s, workspace / ".mcp.json");
    merge_file(s, workspace / ".ode" / "settings.json");
    merge_mcp_file(s, workspace / ".ode" / ".mcp.json");
    if (s.providers.contains(s.provider) == false) {
        s.warnings.push_back("Unknown provider '" + s.provider + "'; using anthropic.");
        s.provider = "anthropic";
    }
    return s;
}

std::string configured_model(const settings& s, const std::string& provider) {
    const auto override = s.model_overrides.find(provider);
    if (override != s.model_overrides.end() && !override->second.empty()) return override->second;
    return s.providers.at(provider).default_model;
}

std::optional<std::string> resolve_provider(const settings& s, const std::string& name, provider_config& out) {
    const auto it = s.providers.find(name);
    if (it == s.providers.end()) return "Unknown provider: " + name;
    const auto& e = it->second;
    out.name = e.name;
    out.kind = e.kind;
    out.base_url = e.base_url;

    // Priority 1: Direct API key in config file
    if (!e.api_key.empty()) {
        out.api_key = e.api_key;
        return std::nullopt;
    }

    // Priority 2: Environment variable
    if (!e.api_key_env.empty() && !env(e.api_key_env).empty()) {
        out.api_key = env(e.api_key_env);
        return std::nullopt;
    }

    // Priority 3: Auth token environment variable
    if (!e.auth_token_env.empty() && !env(e.auth_token_env).empty()) {
        out.api_key = env(e.auth_token_env);
        out.bearer = true;
        return std::nullopt;
    }

    // Priority 4: Windows Credential Manager
#ifdef _WIN32
    PCREDENTIALW cred = nullptr;
    const auto target = widen("ode/" + name);
    if (CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &cred)) {
        std::unique_ptr<CREDENTIALW, decltype(&CredFree)> owner(cred, CredFree);
        out.api_key.assign(reinterpret_cast<const char*>(cred->CredentialBlob), cred->CredentialBlobSize);
        return std::nullopt;
    }
#endif

    // Priority 5: Unauthenticated (for local servers)
    if (e.api_key_env.empty() && e.api_key.empty()) return std::nullopt;

    // No API key found
    return "No API key for " + name + ". Set " + e.api_key_env + " or run: ode login " + name;
}

bool save_credential(const std::string& provider, const std::string& key) {
#ifdef _WIN32
    if (key.empty() || key.size() > CRED_MAX_CREDENTIAL_BLOB_SIZE) return false;
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

bool save_provider_selection(const std::string& provider, const std::string& previous_provider, const fs::path& settings_path) {
    json data = json::object();
    if (const auto existing = read_file(settings_path)) {
        data = json::parse(*existing, nullptr, false, true);
        if (data.is_discarded() || !data.is_object()) return false;
    }
    const auto model = data.find("model");
    const auto model_provider = data.find("model_provider");
    if (model != data.end() && model->is_string() &&
        (model_provider == data.end() || !model_provider->is_string())) {
        const auto selected = data.find("provider");
        data["model_provider"] = selected != data.end() && selected->is_string() ? selected->get<std::string>() : previous_provider;
    }
    data["provider"] = provider;

    std::error_code ec;
    fs::create_directories(settings_path.parent_path(), ec);
    if (ec) return false;
    auto temporary = settings_path;
    temporary += ".tmp";
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out << data.dump(2) << '\n';
        out.flush();
        if (!out) {
            fs::remove(temporary, ec);
            return false;
        }
    }
    if (!MoveFileExW(temporary.c_str(), settings_path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        fs::remove(temporary, ec);
        return false;
    }
    return true;
}

bool delete_credential(const std::string& provider) {
    const auto target = widen("ode/" + provider);
    return CredDeleteW(target.c_str(), CRED_TYPE_GENERIC, 0) != 0 || GetLastError() == ERROR_NOT_FOUND;
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
