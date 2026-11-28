#include "config/config.hpp"
#include "base/files.hpp"
#include "base/text.hpp"
#include "base/utf.hpp"
#include "platform/file_lock.hpp"

#include <algorithm>
#include <cstdlib>
#include <fstream>

#include <wincred.h>
#include <winhttp.h>

namespace ode {
namespace fs = std::filesystem;
namespace {

enum class connection_scope { ignored, user, workspace };

void merge_connection(settings& s, const json& j, connection_scope scope) {
    if (j.contains("provider") && j["provider"].is_string()) s.provider = j["provider"];
    if (j.contains("providers") && j["providers"].is_object()) {
        for (const auto& [name, p] : j["providers"].items()) {
            if (!p.is_object()) continue;
            auto entry = s.providers.find(name);
            if (scope == connection_scope::user) {
                entry = s.providers.try_emplace(name).first;
                auto& e = entry->second;
                e.name = name;
                if (const auto protocol_name = p.find("protocol"); protocol_name != p.end() && protocol_name->is_string()) {
                    if (auto k = parse_protocol(protocol_name->get_ref<const std::string&>())) e.kind = *k;
                }
                if (const auto url = p.find("base_url"); url != p.end() && url->is_string()) {
                    e.base_url = url->get_ref<const std::string&>();
                }
                if (const auto key = p.find("api_key"); key != p.end() && key->is_string()) {
                    e.api_key = key->get_ref<const std::string&>();
                }
                const auto uses_legacy_auth = [&](std::string_view field) {
                    const auto value = p.find(field);
                    return value != p.end() && value->is_string() && !value->get_ref<const std::string&>().empty();
                };
                if (const auto auth = p.find("auth"); auth != p.end()) {
                    if (*auth == "api-key") e.auth = credential_mode::api_key;
                    else if (*auth == "bearer") e.auth = credential_mode::bearer;
                    else if (*auth == "none") e.auth = credential_mode::none;
                    else s.warnings.push_back("Invalid auth mode for provider '" + name + "'.");
                } else if (e.auth == credential_mode::optional &&
                           (uses_legacy_auth("api_key_env") || uses_legacy_auth("auth_token_env"))) {
                    e.auth = credential_mode::api_key;
                }
                if (p.contains("api_key_env") || p.contains("auth_token_env")) {
                    s.warnings.push_back("Key environment variable settings ignored for '" + name + "'; use api_key or ode login " + name + ".");
                }
            }
            if (entry == s.providers.end()) continue;
            if (const auto model = p.find("model"); model != p.end() && model->is_string()) {
                entry->second.default_model = model->get<std::string>();
                s.model_overrides.erase(name);
            }
        }
    }
    if (j.contains("model") && j["model"].is_string()) {
        const auto provider = j.find("model_provider");
        const auto& owner = provider != j.end() && provider->is_string() ? provider->get_ref<const std::string&>() : s.provider;
        s.model_overrides[owner] = j["model"].get<std::string>();
    }
}

void merge(settings& s, const json& j, connection_scope scope = connection_scope::ignored) {
    if (scope != connection_scope::ignored) merge_connection(s, j, scope);
    if (j.contains("env") && j["env"].is_object()) {
        for (const auto& [k, v] : j["env"].items()) {
            if (v.is_string()) _putenv_s(k.c_str(), v.get<std::string>().c_str());
        }
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

void merge_file(settings& s, const fs::path& path, connection_scope scope = connection_scope::ignored) {
    std::error_code ec;
    if (!fs::is_regular_file(path, ec)) return;
    const auto text = read_file(path);
    auto j = json::parse(text.value_or(""), nullptr, false, true);
    if (j.is_discarded() || !j.is_object()) {
        s.warnings.push_back("Ignored invalid JSON in " + path.string());
        return;
    }
    merge(s, j, scope);
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
    static constexpr std::pair<std::string_view, std::string_view> aliases[] = {
        {"opus", "claude-opus-4-8"},
        {"sonnet", "claude-sonnet-4-6"},
        {"haiku", "claude-haiku-4-5-20251001"},
        {"fable", "claude-fable-5"},
    };
    // "opus[1m]" keeps its window suffix after resolution.
    const auto bracket = model.find('[');
    const auto base = model.substr(0, bracket);
    const auto suffix = bracket == std::string::npos ? std::string{} : model.substr(bracket);
    for (const auto& [name, id] : aliases) {
        if (base == name) return std::string(id) + suffix;
    }
    return model;
}

settings load_settings(const fs::path& workspace) {
    settings s;
    s.providers["anthropic"] = {"anthropic", protocol::anthropic, "https://api.anthropic.com",
                                "claude-sonnet-4-6", credential_mode::api_key};
    s.providers["openai"] = {"openai", protocol::openai_chat, "https://api.openai.com/v1",
                             "gpt-5", credential_mode::api_key};
    s.providers["deepseek"] = {"deepseek", protocol::openai_chat, "https://api.deepseek.com/v1",
                               "deepseek-flash", credential_mode::api_key};
    const auto home = user_home();
    const auto user_dir = home / ".ode";
    if (home.is_absolute()) s.user_settings_path = user_dir / "settings.json";
    // Later files override earlier ones. .mcp.json is Claude Code's server list and is
    // read at each of the same scopes, after that scope's settings.json.
    merge_file(s, home / ".claude" / "settings.json");
    merge_mcp_file(s, home / ".claude" / ".mcp.json");
    merge_file(s, user_dir / "settings.json", home.is_absolute() ? connection_scope::user : connection_scope::ignored);
    merge_mcp_file(s, ode_home() / ".mcp.json");
    merge_file(s, workspace / ".claude" / "settings.json");
    merge_file(s, workspace / ".claude" / "settings.local.json");
    merge_mcp_file(s, workspace / ".mcp.json");
    merge_file(s, workspace / ".ode" / "settings.json", connection_scope::workspace);
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

namespace {

std::optional<json> read_store(const fs::path& path) {
    const auto text = read_file(path);
    if (!text) {
        std::error_code ec;
        if (fs::exists(path, ec) || ec) return std::nullopt;
        return json::object();
    }
    auto data = json::parse(*text, nullptr, false, true);
    if (!data.is_object()) return std::nullopt;
    return data;
}

bool write_store(const fs::path& path, const json& data) {
    auto temporary = path;
    temporary += ".tmp";
    std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
    out << data.dump(2) << '\n';
    out.close();
    if (out && MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        return true;
    }
    std::error_code ec;
    fs::remove(temporary, ec);
    return false;
}

template <class Update>
bool update_store(const fs::path& path, Update update) {
    if (path.empty()) return false;
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    if (ec) return false;
    auto lock_path = path;
    lock_path += ".lock";
    const file_lock lock(lock_path);
    if (!lock) return false;
    auto data = read_store(path);
    if (!data) return false;
    const auto changed = update(*data);
    return changed && (!*changed || write_store(path, *data));
}

bool valid_api_key(std::string_view key) {
    return !key.empty() && key.find_first_of("\r\n") == std::string_view::npos && key.find('\0') == std::string_view::npos;
}

void select_provider(json& data, const std::string& provider) {
    const auto model = data.find("model");
    const auto model_provider = data.find("model_provider");
    if (model != data.end() && model->is_string() &&
        (model_provider == data.end() || !model_provider->is_string())) {
        const auto selected = data.find("provider");
        data["model_provider"] = selected != data.end() && selected->is_string() ? selected->get<std::string>() : "anthropic";
    }
    data["provider"] = provider;
}

} // namespace

std::optional<std::string> resolve_provider(const settings& s, const std::string& name, provider_config& out) {
    const auto it = s.providers.find(name);
    if (it == s.providers.end()) return "Unknown provider: " + name;
    const auto& e = it->second;
    out = {};
    out.name = e.name;
    out.kind = e.kind;
    out.base_url = e.base_url;
    out.bearer = e.auth == credential_mode::bearer;
    if (e.base_url.empty()) return "No base_url for " + name + " in user .ode/settings.json.";
    if (e.auth == credential_mode::none) return std::nullopt;
    if (!e.api_key.empty()) {
        if (!valid_api_key(e.api_key)) return "Invalid api_key for " + name + " in user .ode/settings.json.";
        out.api_key = e.api_key;
        return std::nullopt;
    }
    if (e.auth == credential_mode::optional) return std::nullopt;
    return "No API key for " + name + "; run ode login " + name + ".";
}

bool save_credential(settings& s, const std::string& provider, const std::string& key) {
    const auto it = s.providers.find(provider);
    if (it == s.providers.end() || it->second.auth == credential_mode::none ||
        it->second.base_url.empty() || !valid_api_key(key)) return false;
    if (!update_store(s.user_settings_path, [&](json& data) -> std::optional<bool> {
        auto& providers = data["providers"];
        if (!providers.is_null() && !providers.is_object()) return std::nullopt;
        auto& entry = providers[provider];
        if (!entry.is_null() && !entry.is_object()) return std::nullopt;
        entry["api_key"] = key;
        return true;
    })) return false;
    it->second.api_key = key;
    return true;
}

bool delete_credential(settings& s, const std::string& provider) {
    if (!update_store(s.user_settings_path, [&](json& data) -> std::optional<bool> {
        const auto providers = data.find("providers");
        if (providers == data.end() || !providers->is_object()) return false;
        const auto entry = providers->find(provider);
        return entry != providers->end() && entry->is_object() && entry->erase("api_key") != 0;
    })) return false;
    if (const auto it = s.providers.find(provider); it != s.providers.end()) it->second.api_key.clear();
    return true;
}

bool import_windows_credential(settings& s, const std::string& provider) {
    const auto target = widen("ode/" + provider);
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(target.c_str(), CRED_TYPE_GENERIC, 0, &credential)) return false;
    std::string key(reinterpret_cast<const char*>(credential->CredentialBlob), credential->CredentialBlobSize);
    CredFree(credential);
    const bool saved = save_credential(s, provider, key);
    SecureZeroMemory(key.data(), key.size());
    return saved;
}

bool save_provider_selection(const std::string& provider, const fs::path& settings_path) {
    return update_store(settings_path, [&](json& data) -> std::optional<bool> {
        select_provider(data, provider);
        return true;
    });
}

bool valid_provider_id(std::string_view name) {
    constexpr std::string_view alphanumeric = "abcdefghijklmnopqrstuvwxyz0123456789";
    return !name.empty() && alphanumeric.find(name.front()) != std::string_view::npos &&
           name.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789_-") == std::string_view::npos;
}

bool normalize_provider_base_url(protocol kind, std::string& url) {
    if ((!url.starts_with("https://") && !url.starts_with("http://")) ||
        url.find_first_of("?#\\") != std::string::npos ||
        std::ranges::any_of(url, [](unsigned char c) { return c <= ' ' || c == 127; })) return false;
    const auto wide = widen(url);
    URL_COMPONENTS parts{};
    parts.dwStructSize = sizeof(parts);
    parts.dwHostNameLength = parts.dwUrlPathLength = parts.dwUserNameLength =
        parts.dwPasswordLength = static_cast<DWORD>(-1);
    if (!WinHttpCrackUrl(wide.c_str(), 0, 0, &parts) || parts.dwHostNameLength == 0 ||
        parts.dwUserNameLength != 0 || parts.dwPasswordLength != 0) return false;

    const auto path_start = url.find('/', url.find("://") + 3);
    if (url.find('@', url.find("://") + 3) < path_start) return false;
    auto end = url.find_last_not_of('/');
    if (path_start != std::string::npos && end >= path_start) {
        const auto path = std::string_view(url).substr(path_start, end - path_start + 1);
        if (path.ends_with("/chat/completions") || path.ends_with("/responses") ||
            path.ends_with("/messages") || path.ends_with("/models")) return false;
        if (kind == protocol::anthropic && path.ends_with("/v1")) {
            end -= 3;
            while (url[end] == '/') --end;
        }
    }
    url.resize(end + 1);
    return true;
}

bool save_new_provider(settings& s, const provider_entry& entry) {
    auto saved = entry;
    if (!valid_provider_id(saved.name) || s.providers.contains(saved.name) ||
        !normalize_provider_base_url(saved.kind, saved.base_url) ||
        saved.default_model.empty() ||
        std::ranges::any_of(saved.default_model, [](unsigned char c) { return c <= ' ' || c == 127; }) ||
        (saved.auth != credential_mode::api_key && saved.auth != credential_mode::bearer) ||
        !valid_api_key(saved.api_key)) return false;
    if (!update_store(s.user_settings_path, [&](json& data) -> std::optional<bool> {
        auto& providers = data["providers"];
        if ((!providers.is_null() && !providers.is_object()) || providers.contains(saved.name)) return std::nullopt;
        providers[saved.name] = {{"protocol", protocol_name(saved.kind)}, {"base_url", saved.base_url},
                                {"model", saved.default_model}, {"api_key", saved.api_key},
                                {"auth", saved.auth == credential_mode::bearer ? "bearer" : "api-key"}};
        select_provider(data, saved.name);
        return true;
    })) return false;
    s.providers.emplace(saved.name, std::move(saved));
    return true;
}

bool save_model_selection(const std::string& provider, const std::string& model, const fs::path& settings_path) {
    if (provider.empty() || model.empty()) return false;
    return update_store(settings_path, [&](json& data) -> std::optional<bool> {
        data["model"] = model;
        data["model_provider"] = provider;
        return true;
    });
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
