#include "app/app.hpp"
#include "base/files.hpp"
#include "provider/models.hpp"

#include <doctest.h>
#include <cstdlib>
#include <fstream>
#include <random>

using namespace ode;

namespace {

struct temp_directory {
    fs::path path = fs::temp_directory_path() / ("ode-provider-" + std::to_string(std::random_device{}()));
    ~temp_directory() {
        std::error_code ec;
        fs::remove_all(path, ec);
    }
};

struct scoped_env {
    const char* name;
    std::string previous;
    scoped_env(const char* variable, const std::string& value) : name(variable) {
        if (const auto* old = std::getenv(name)) previous = old;
        _putenv_s(name, value.c_str());
    }
    ~scoped_env() { _putenv_s(name, previous.c_str()); }
};

void write_settings(const fs::path& path, std::string_view content) {
    fs::create_directories(path.parent_path());
    std::ofstream(path) << content;
}

} // namespace

TEST_CASE("provider selection persists without replacing other settings") {
    const auto directory = fs::temp_directory_path() /
        ("ode-provider-selection-" + std::to_string(std::random_device{}()));
    fs::create_directories(directory);
    const auto path = directory / "settings.json";
    {
        std::ofstream out(path);
        out << R"({"model":"custom-model","providers":{"deepseek":{"base_url":"https://gateway"}}})";
    }

    REQUIRE(save_provider_selection("deepseek", path));
    REQUIRE(save_provider_selection("deepseek", path));
    const auto saved = json::parse(*read_file(path));
    CHECK(saved["provider"] == "deepseek");
    CHECK(saved["model"] == "custom-model");
    CHECK(saved["model_provider"] == "anthropic");
    CHECK(saved["providers"]["deepseek"]["base_url"] == "https://gateway");
    CHECK_FALSE(saved.contains("api_key"));
    fs::remove_all(directory);
}

TEST_CASE("third-party provider setup saves the definition and selection together") {
    temp_directory dir;
    const auto home = dir.path / "home";
    const auto workspace = dir.path / "project";
    scoped_env profile("USERPROFILE", home.string());
    const auto store = home / ".ode" / "settings.json";
    write_settings(store, R"({"provider":"openai","model":"old-model","extra":17})");
    auto s = load_settings(workspace);
    auto stale = s;
    const provider_entry gateway{"gateway", protocol::anthropic, "https://gateway.example/api/v1/",
                                 "vendor/model", credential_mode::bearer, "fixture-key"};
    REQUIRE(save_new_provider(s, gateway));
    CHECK(s.provider == "openai");
    CHECK_FALSE(save_new_provider(stale, gateway));
    CHECK_FALSE(stale.providers.contains("gateway"));

    const auto data = json::parse(*read_file(store));
    CHECK(data["provider"] == "gateway");
    CHECK(data["model_provider"] == "openai");
    CHECK(data["extra"] == 17);
    CHECK(data["providers"]["gateway"]["base_url"] == "https://gateway.example/api");
    const auto loaded = load_settings(workspace);
    CHECK(loaded.provider == "gateway");
    CHECK(configured_model(loaded, "gateway") == "vendor/model");
    CHECK(configured_model(loaded, "openai") == "old-model");
    provider_config connection;
    REQUIRE_FALSE(resolve_provider(loaded, "gateway", connection));
    CHECK(connection.bearer);
    CHECK(connection.api_key == "fixture-key");
    CHECK(models_url(connection) == "https://gateway.example/api/v1/models");

    write_settings(store, "{");
    auto unsaved = gateway;
    unsaved.name = "other";
    CHECK_FALSE(save_new_provider(s, unsaved));
    CHECK_FALSE(s.providers.contains("other"));
    CHECK(read_file(store) == "{");
}

TEST_CASE("provider setup accepts API roots and rejects complete endpoint URLs") {
    auto chat = std::string("https://gateway.example/api/v1///");
    REQUIRE(normalize_provider_base_url(protocol::openai_chat, chat));
    CHECK(chat == "https://gateway.example/api/v1");
    auto responses = std::string("http://127.0.0.1:8123/api");
    REQUIRE(normalize_provider_base_url(protocol::openai_responses, responses));
    provider_config connection;
    connection.kind = protocol::openai_responses;
    connection.base_url = responses;
    CHECK(models_url(connection) == "http://127.0.0.1:8123/api/v1/models");

    for (auto url : {std::string("gateway.example"), std::string("https://user@host.example"),
                     std::string("https://host.example/v1/responses"), std::string("https://host.example?v=1")}) {
        CHECK_FALSE(normalize_provider_base_url(protocol::openai_responses, url));
    }
}

TEST_CASE("model selection persists with its provider") {
    const auto directory = fs::temp_directory_path() /
        ("ode-model-selection-" + std::to_string(std::random_device{}()));
    fs::create_directories(directory);
    const auto path = directory / "settings.json";
    REQUIRE(save_model_selection("gateway", "custom-model", path));
    const auto saved = json::parse(*read_file(path));
    CHECK(saved["model"] == "custom-model");
    CHECK(saved["model_provider"] == "gateway");
    fs::remove_all(directory);
}

TEST_CASE("official provider model context windows use provider metadata") {
    provider_config anthropic;
    anthropic.name = "anthropic";
    CHECK(known_model_context_window(anthropic, "claude-opus-4-8") == 200000);
    CHECK(known_model_context_window(anthropic, "claude-sonnet-4-6") == 200000);

    provider_config openai;
    openai.name = "openai";
    CHECK(known_model_context_window(openai, "gpt-5") == 400000);
    CHECK(known_model_context_window(openai, "gpt-5-mini") == 400000);
    CHECK(known_model_context_window(openai, "gpt-4.1") == 1047576);
    CHECK(known_model_context_window(openai, "gpt-4.1-mini") == 1047576);
    CHECK(known_model_context_window(openai, "gpt-4o") == 128000);
}

TEST_CASE("DeepSeek official models use the documented context window") {
    provider_config deepseek;
    deepseek.name = "deepseek";
    CHECK(known_model_context_window(deepseek, "deepseek-flash") == 1048576);
    CHECK(known_model_context_window(deepseek, "deepseek-v4-pro") == 1048576);
    CHECK(known_model_context_window(deepseek, "deepseek-coder") == 0);
}

TEST_CASE("custom gateways do not infer context windows from model names") {
    provider_config gateway;
    gateway.name = "ustc";
    gateway.kind = protocol::openai_responses;
    CHECK(known_model_context_window(gateway, "deepseek-flash") == 0);
    CHECK(known_model_context_window(gateway, "deepseek-v4-flash") == 0);
}

TEST_CASE("unreported custom model context uses the 272k default") {
    app current;
    current.config.providers["gateway"] = {
        "gateway", protocol::openai_responses, "https://gateway.example/v1", "custom-model", credential_mode::none};
    current.provider_cfg.name = "gateway";
    auto provider = make_provider(current.provider_cfg);
    current.model_provider = std::move(provider);
    current.core = std::make_unique<agent>(*current.model_provider, std::vector<tool>{},
                                           current.permissions, fs::current_path());
    current.set_model("custom-model");
    CHECK(current.core->context_window == 272000);
}

TEST_CASE("switching providers updates the live client and default model") {
    temp_directory dir;
    app current;
    current.config.user_settings_path = dir.path / "settings.json";
    current.config.providers["anthropic"] = {
        "anthropic", protocol::anthropic, "https://api.anthropic.com", "claude-sonnet-4-6", credential_mode::api_key};
    current.config.providers["deepseek"] = {
        "deepseek", protocol::openai_chat, "https://api.deepseek.com", "deepseek-flash", credential_mode::api_key};
    REQUIRE(save_credential(current.config, "anthropic", "anthropic-key"));
    REQUIRE(save_credential(current.config, "deepseek", "deepseek-key"));

    REQUIRE_FALSE(resolve_provider(current.config, "anthropic", current.provider_cfg));
    current.model_provider = make_provider(current.provider_cfg);
    const auto* client = current.model_provider.get();
    current.core = std::make_unique<agent>(*current.model_provider, std::vector<tool>{}, current.permissions, fs::current_path());
    current.config.model_overrides["anthropic"] = "custom-anthropic-model";
    current.set_model("claude-opus-4-8");

    CHECK_FALSE(current.use_provider("deepseek"));
    CHECK(current.model_provider.get() == client);
    CHECK(current.config.provider == "deepseek");
    CHECK(current.provider_cfg.name == "deepseek");
    CHECK(current.provider_cfg.kind == protocol::openai_chat);
    CHECK(current.provider_cfg.base_url == "https://api.deepseek.com");
    CHECK(current.provider_cfg.api_key == "deepseek-key");
    CHECK(known_model_context_window(current.provider_cfg, "deepseek-v4-pro") == 1048576);
    CHECK(known_model_context_window(current.provider_cfg, "deepseek-coder") == 0);
    CHECK(current.core->model == "deepseek-flash");
    CHECK(current.core->context_window == 1048576);

    current.core->model = "custom-deepseek-model";
    REQUIRE(save_credential(current.config, "deepseek", "rotated-key"));
    CHECK_FALSE(current.use_provider("deepseek"));
    CHECK(current.provider_cfg.api_key == "rotated-key");
    CHECK(current.core->model == "custom-deepseek-model");

    CHECK_FALSE(current.use_provider("anthropic"));
    CHECK(current.core->model == "custom-anthropic-model");
}

TEST_CASE("provider switching rejects unknown providers without changing the active one") {
    app current;
    current.config.provider = "anthropic";
    current.config.providers["anthropic"] = {
        "anthropic", protocol::anthropic, "https://api.anthropic.com", "claude-sonnet-4-6", credential_mode::none};
    REQUIRE_FALSE(resolve_provider(current.config, "anthropic", current.provider_cfg));
    current.model_provider = make_provider(current.provider_cfg);
    current.core = std::make_unique<agent>(*current.model_provider, std::vector<tool>{}, current.permissions, fs::current_path());
    current.set_model("claude-sonnet-4-6");

    const auto error = current.use_provider("missing");
    REQUIRE(error);
    CHECK(error->find("Unknown provider") != std::string::npos);
    CHECK(current.config.provider == "anthropic");
    CHECK(current.provider_cfg.name == "anthropic");
}

TEST_CASE("only ode settings select the provider and only user settings configure its endpoint") {
    temp_directory dir;
    const auto home = dir.path / "home";
    const auto workspace = dir.path / "project";
    scoped_env profile("USERPROFILE", home.string());
    scoped_env api_key("ANTHROPIC_API_KEY", "ambient-key");
    scoped_env base_url("ANTHROPIC_BASE_URL", "https://ambient.invalid");
    scoped_env alias("ANTHROPIC_DEFAULT_SONNET_MODEL", "ambient-model");
    write_settings(home / ".claude" / "settings.json", R"({"provider":"deepseek","model":"claude-model"})");
    write_settings(workspace / ".claude" / "settings.local.json", R"({
        "provider":"openai","model":"claude-project-model","permissions":{"defaultMode":"plan"},
        "providers":{"anthropic":{"api_key":"claude-key"}}
    })");
    write_settings(home / ".ode" / "settings.json", R"({
        "providers":{"anthropic":{"base_url":"https://user.example", "model":"user-model", "api_key":"legacy-key"},
                     "gateway":{"base_url":"https://user.example", "api_key_env":"ANTHROPIC_API_KEY"}}
    })");
    write_settings(workspace / ".ode" / "settings.json", R"({
        "providers":{"anthropic":{"base_url":"https://project.invalid","protocol":"openai-chat","auth":"none","model":"project-model","api_key":"project-key"},
                     "project-only":{"base_url":"https://project.invalid"}}
    })");

    auto s = load_settings(workspace);
    CHECK(s.provider == "anthropic");
    CHECK(s.permission_mode == "plan");
    CHECK(configured_model(s, "anthropic") == "project-model");
    CHECK(resolve_model_alias("sonnet") == "claude-sonnet-4-6");
    CHECK_FALSE(s.providers.contains("project-only"));
    provider_config resolved;
    const auto gateway_error = resolve_provider(s, "gateway", resolved);
    REQUIRE(gateway_error);
    CHECK(gateway_error->find("ode login gateway") != std::string::npos);
    CHECK(resolved.api_key.empty());
    REQUIRE_FALSE(resolve_provider(s, "anthropic", resolved));
    CHECK(resolved.api_key == "legacy-key");
    REQUIRE(save_credential(s, "anthropic", "stored-key"));
    REQUIRE_FALSE(resolve_provider(s, "anthropic", resolved));
    CHECK(resolved.api_key == "stored-key");
    CHECK(resolved.base_url == "https://user.example");
    CHECK(resolved.kind == protocol::anthropic);

    const auto saved = json::parse(*read_file(s.user_settings_path));
    CHECK(saved["providers"]["anthropic"]["api_key"] == "stored-key");
    CHECK(saved["providers"]["anthropic"]["model"] == "user-model");
    CHECK(saved["providers"]["gateway"]["base_url"] == "https://user.example");
    s = load_settings(workspace);
    REQUIRE_FALSE(resolve_provider(s, "anthropic", resolved));
    CHECK(resolved.api_key == "stored-key");
    REQUIRE(delete_credential(s, "anthropic"));
    const auto updated = json::parse(*read_file(s.user_settings_path));
    CHECK_FALSE(updated["providers"]["anthropic"].contains("api_key"));
    CHECK(updated["providers"]["anthropic"]["base_url"] == "https://user.example");
    CHECK(resolve_provider(load_settings(workspace), "anthropic", resolved).has_value());
    CHECK(resolved.api_key.empty());
}
