#include "app/app.hpp"
#include "base/files.hpp"
#include "provider/models.hpp"

#include <doctest.h>
#include <fstream>
#include <random>

using namespace ode;

TEST_CASE("provider selection persists without replacing other settings") {
    const auto directory = fs::temp_directory_path() /
        ("ode-provider-selection-" + std::to_string(std::random_device{}()));
    fs::create_directories(directory);
    const auto path = directory / "settings.json";
    {
        std::ofstream out(path);
        out << R"({"model":"custom-model","providers":{"deepseek":{"base_url":"https://gateway"}}})";
    }

    REQUIRE(save_provider_selection("deepseek", "anthropic", path));
    REQUIRE(save_provider_selection("deepseek", "deepseek", path));
    const auto saved = json::parse(*read_file(path));
    CHECK(saved["provider"] == "deepseek");
    CHECK(saved["model"] == "custom-model");
    CHECK(saved["model_provider"] == "anthropic");
    CHECK(saved["providers"]["deepseek"]["base_url"] == "https://gateway");
    CHECK_FALSE(saved.contains("api_key"));
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

TEST_CASE("switching providers updates the live client and default model") {
    app current;
    current.config.providers["anthropic"] = {
        "anthropic", protocol::anthropic, "https://api.anthropic.com", "", "", "claude-sonnet-4-6", "anthropic-key"};
    current.config.providers["deepseek"] = {
        "deepseek", protocol::openai_chat, "https://api.deepseek.com", "", "", "deepseek-flash", "deepseek-key"};

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
    current.config.providers["deepseek"].api_key = "rotated-key";
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
        "anthropic", protocol::anthropic, "https://api.anthropic.com", "", "", "claude-sonnet-4-6", "key"};
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
