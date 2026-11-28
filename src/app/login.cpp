#include "app/interactive.hpp"

#include <algorithm>
#include <array>

namespace ode {
namespace {

constexpr std::array builtin_names{"deepseek", "openai", "anthropic"};
constexpr std::array builtin_labels{"DeepSeek", "OpenAI", "Anthropic"};
constexpr std::array protocols{protocol::openai_chat, protocol::openai_responses, protocol::anthropic};

std::string provider_status(const app& a, const std::string& name) {
    const auto& entry = a.config.providers.at(name);
    provider_config connection;
    std::string status;
    if (entry.base_url.empty()) status = "缺少 Base URL";
    else if (resolve_provider(a.config, name, connection)) status = "需要 API Key";
    else status = connection.api_key.empty() ? "无需密钥" : "已配置";
    if (a.config.provider == name) status += " · 当前";
    return status;
}

} // namespace

void interactive_ui::show_login_methods() {
    selection_ = {{"使用 API Key"}, {}, 0, "选择登录方式",
                  [this](std::size_t) { show_login_providers(); }, {}};
}

void interactive_ui::show_login_providers() {
    selection_state menu;
    menu.title = "选择服务商";
    for (std::size_t i = 0; i < builtin_names.size(); ++i) {
        menu.items.emplace_back(builtin_labels[i]);
        menu.details.push_back(provider_status(app_, builtin_names[i]));
    }
    menu.items.emplace_back("第三方服务商");
    menu.on_select = [this](std::size_t index) {
        if (index == builtin_names.size()) show_custom_providers();
        else select_login_provider(builtin_names[index], [this] { show_login_providers(); });
    };
    menu.on_cancel = [this] { show_login_methods(); };
    selection_ = std::move(menu);
}

void interactive_ui::show_custom_providers() {
    setup_.reset();
    selection_state menu;
    menu.title = "第三方服务商";
    menu.items.emplace_back("＋ 添加第三方服务商");
    menu.details.emplace_back();
    std::vector<std::string> names;
    for (const auto& [name, entry] : app_.config.providers) {
        if (std::ranges::find(builtin_names, name) != builtin_names.end()) continue;
        names.push_back(name);
        menu.items.push_back(name);
        menu.details.push_back(provider_status(app_, name));
    }
    menu.on_select = [this, names = std::move(names)](std::size_t index) {
        if (index == 0) {
            setup_.emplace();
            setup_->entry.kind = protocol::openai_chat;
            setup_->entry.auth = credential_mode::api_key;
            show_setup_field(setup_field::name);
        } else {
            select_login_provider(names[index - 1], [this] { show_custom_providers(); });
        }
    };
    menu.on_cancel = [this] { show_login_providers(); };
    selection_ = std::move(menu);
}

void interactive_ui::select_login_provider(const std::string& name, std::function<void()> on_back) {
    if (switch_to_saved_provider(name)) return;
    const auto& entry = app_.config.providers.at(name);
    if (entry.base_url.empty()) {
        note("请在用户 .ode/settings.json 配置 " + name + " 的 base_url", ui::tone::error);
        on_back();
    } else {
        login_ = login_state{name, {}, std::move(on_back)};
    }
}

void interactive_ui::show_setup_field(setup_field field) {
    setup_->field = field;
    setup_->error.clear();
    const auto& entry = setup_->entry;
    switch (field) {
        case setup_field::name: setup_->input.set(entry.name); break;
        case setup_field::base_url: setup_->input.set(entry.base_url); break;
        case setup_field::api_key: setup_->input.set(entry.api_key); break;
        case setup_field::model: setup_->input.set(entry.default_model); break;
    }
}

void interactive_ui::show_setup_protocols() {
    setup_->field.reset();
    const auto index = std::ranges::find(protocols, setup_->entry.kind) - protocols.begin();
    selection_ = {{"OpenAI Chat Completions", "OpenAI Responses", "Anthropic Messages"}, {},
                  static_cast<std::size_t>(index), "选择 API 协议",
                  [this](std::size_t selected) {
                      if (setup_->entry.kind != protocols[selected]) {
                          setup_->entry.api_key.clear();
                          setup_->entry.auth = credential_mode::api_key;
                      }
                      setup_->entry.kind = protocols[selected];
                      show_setup_field(setup_field::base_url);
                  },
                  [this] { show_setup_field(setup_field::name); }};
}

void interactive_ui::show_setup_auth() {
    setup_->field.reset();
    selection_ = {{"x-api-key", "Authorization: Bearer"}, {},
                  setup_->entry.auth == credential_mode::bearer ? 1u : 0u, "选择 Anthropic 鉴权方式",
                  [this](std::size_t index) {
                      setup_->entry.auth = index == 0 ? credential_mode::api_key : credential_mode::bearer;
                      show_setup_field(setup_field::api_key);
                  },
                  [this] { show_setup_field(setup_field::base_url); }};
}

void interactive_ui::handle_provider_setup(const ui::key& k) {
    using ui::key_kind;
    if (k.kind == key_kind::ctrl && k.letter == 'c') {
        if (setup_->fetching) cancel();
        setup_.reset();
        return;
    }
    if (setup_->fetching) {
        if (k.kind == key_kind::escape) cancel();
        return;
    }

    auto& input = setup_->input;
    switch (k.kind) {
        case key_kind::escape:
            input.clear();
            switch (*setup_->field) {
                case setup_field::name: show_custom_providers(); break;
                case setup_field::base_url: show_setup_protocols(); break;
                case setup_field::api_key:
                    if (setup_->entry.kind == protocol::anthropic) show_setup_auth();
                    else show_setup_field(setup_field::base_url);
                    break;
                case setup_field::model: show_setup_models(); break;
            }
            break;
        case key_kind::enter: submit_setup_field(); break;
        case key_kind::character:
            if (k.text.find_first_of("\r\n") == std::string::npos) input.insert(k.text);
            break;
        case key_kind::backspace: input.backspace(); break;
        case key_kind::del: input.erase_forward(); break;
        case key_kind::left: input.left(); break;
        case key_kind::right: input.right(); break;
        case key_kind::home: input.home(); break;
        case key_kind::end: input.end(); break;
        default: break;
    }
}

void interactive_ui::submit_setup_field() {
    auto& entry = setup_->entry;
    auto value = setup_->input.text();
    switch (*setup_->field) {
        case setup_field::name:
            if (!valid_provider_id(value)) {
                setup_->error = "请输入以小写字母或数字开头的服务商 ID，可含 - 和 _";
            } else if (app_.config.providers.contains(value)) {
                setup_->error = "该服务商 ID 已存在";
            } else {
                entry.name = std::move(value);
                show_setup_protocols();
            }
            break;
        case setup_field::base_url:
            if (!normalize_provider_base_url(entry.kind, value)) {
                setup_->error = "请输入 HTTP(S) 基础地址，可含路径，例如 https://example.com/api/v1";
            } else {
                if (entry.base_url != value) entry.api_key.clear();
                entry.base_url = std::move(value);
                if (entry.kind == protocol::anthropic) show_setup_auth();
                else show_setup_field(setup_field::api_key);
            }
            break;
        case setup_field::api_key:
            if (value.empty() || std::ranges::any_of(value, [](unsigned char c) { return c <= ' ' || c == 127; })) {
                setup_->error = "请输入不含空白的 API Key";
            } else {
                entry.api_key = std::move(value);
                setup_->input.clear();
                fetch_setup_models();
            }
            break;
        case setup_field::model:
            if (value.empty() || std::ranges::any_of(value, [](unsigned char c) { return c <= ' ' || c == 127; })) {
                setup_->error = "请输入服务商提供的模型 ID";
            } else {
                entry.default_model = std::move(value);
                confirm_provider_setup();
            }
            break;
    }
}

void interactive_ui::fetch_setup_models() {
    setup_->field.reset();
    setup_->fetching = true;
    const auto& entry = setup_->entry;
    provider_config connection{entry.name, entry.kind, entry.base_url, entry.api_key,
                               entry.auth == credential_mode::bearer, {}};
    start_job([this, connection = std::move(connection)](agent_events&, cancel_token& cancel) {
        setup_models_result_ = list_models(connection, &cancel);
        return cancel.cancelled() ? turn_result{stop_reason::cancelled, {}} : turn_result{};
    }, false, false);
}

void interactive_ui::finish_setup_models() {
    auto result = std::exchange(setup_models_result_, std::nullopt);
    if (!setup_ || !setup_->fetching) return;
    setup_->fetching = false;
    if (cancel_.cancelled()) {
        show_setup_field(setup_field::api_key);
    } else {
        setup_->available = std::move(result).value_or(model_list{{}, "无法获取模型列表"});
        show_setup_models();
    }
}

void interactive_ui::show_setup_models() {
    setup_->field.reset();
    const auto& available = setup_->available;
    selection_state menu;
    menu.title = available.error.empty() ? "选择默认模型" : available.error;
    if (!available.models.empty()) menu.search.emplace();
    for (const auto& model : available.models) {
        menu.items.push_back(model.id);
        menu.details.push_back(model.name == model.id ? std::string{} : model.name);
    }
    menu.items.emplace_back("手动输入模型 ID");
    menu.items.emplace_back("修改连接并重试");
    menu.on_select = [this](std::size_t index) {
        const auto& models = setup_->available.models;
        if (index == models.size()) {
            show_setup_field(setup_field::model);
        } else if (index > models.size()) {
            show_setup_field(setup_field::base_url);
        } else {
            setup_->entry.default_model = models[index].id;
            confirm_provider_setup();
        }
    };
    menu.on_cancel = [this] { show_setup_field(setup_field::api_key); };
    selection_ = std::move(menu);
}

void interactive_ui::confirm_provider_setup() {
    setup_->field.reset();
    const auto& entry = setup_->entry;
    note("服务商 " + entry.name + " · " + std::string(protocol_name(entry.kind)));
    note("Base URL " + entry.base_url);
    note("默认模型 " + entry.default_model);
    selection_ = {{"保存并切换"}, {}, 0, "确认第三方服务商",
                  [this](std::size_t) {
                      const auto name = setup_->entry.name;
                      if (!save_new_provider(app_.config, setup_->entry)) {
                          note("无法保存服务商，请检查用户设置文件和服务商 ID", ui::tone::error);
                          show_setup_models();
                          return;
                      }
                      setup_.reset();
                      if (const auto error = app_.use_provider(name)) {
                          note(*error, ui::tone::error);
                          return;
                      }
                      note("已切换到 " + name + "，模型 " + app_.core->model, ui::tone::accent);
                  },
                  [this] { show_setup_models(); }};
}

} // namespace ode
