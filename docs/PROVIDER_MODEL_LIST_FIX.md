# Provider 自动获取模型功能修复

## 问题

provider 接入时不能正常响应，`ode models` 命令显示错误的硬编码模型列表，而不是从 API 实际获取的模型。

## 根本原因

在 `src/main.cpp` 中，`ode models` 命令在 `provider_cfg` 初始化**之前**被调用，导致 `list_models()` 收到的是空的 provider 配置。

```cpp
// 错误的顺序
if (list_models) {
    return cmd_models(a, "");  // 此时 a.provider_cfg 还未初始化
}

if (auto error = resolve_provider(..., a.provider_cfg)) return fail(*error);  // 在这里才初始化
```

## 修复内容

### 1. main.cpp - 修复初始化顺序

**文件:** `src/main.cpp:147-169`

将 `resolve_provider` 调用移到 `list_models` 检查之前：

```cpp
// 修复后的顺序
if (auto error = resolve_provider(a.config, a.config.provider, a.provider_cfg)) return fail(*error);

if (list_models) {
    return cmd_models(a, "");  // 现在 a.provider_cfg 已正确初始化
}
```

### 2. models.cpp - 改进模型列表获取

**文件:** `src/provider/models.cpp`

#### 改进 1: Anthropic 协议也尝试调用 API

```cpp
case protocol::anthropic:
    // 对于代理服务（如 PassionAPI），尝试调用 /v1/models
    // 如果失败，回退到硬编码列表
    {
        std::string base = config.base_url;
        while (!base.empty() && base.back() == '/') base.pop_back();
        req.url = base + "/v1/models";
        req.method = "GET";
        
        if (!config.api_key.empty()) {
            if (config.bearer) {
                req.headers.emplace_back("authorization", "Bearer " + config.api_key);
            } else {
                req.headers.emplace_back("x-api-key", config.api_key);
            }
        }
        
        // ... 解析响应 ...
        
        // 如果 API 调用失败，使用 fallback
        if (models.empty()) {
            models = {/* 硬编码列表 */};
        }
    }
    break;
```

#### 改进 2: OpenAI 协议使用 GET 方法

```cpp
case protocol::openai_chat:
case protocol::openai_responses: {
    std::string base = config.base_url;
    while (!base.empty() && base.back() == '/') base.pop_back();
    if (!base.ends_with("/v1")) base += "/v1";
    req.method = "GET";  // 明确设置 GET 方法
    req.url = base + "/models";
    // ...
}
```

## 测试结果

### 模型列表获取 ✅

```bash
$ ode models
Fetching models from passion...

Available models:

  codex-auto-review
  gpt-5.6-luna
  gpt-5.6-sol
  gpt-5.6-terra
  gpt-6-astra
  gpt-6-sol
  gpt-6.1-sol
  gpt-6.1-terra
  gpt-6.2-astra
  gpt-6.2-sol
  gpt-image-2
  gpt-image-2.5
  gpt-image-2.5-flare
  gpt-image-2.5-sunburst
  gpt-image-2.6
  gpt-image-2.6-flare
  gpt-image-2.6-sunburst

Use: ode --model <model-id>
```

### API 调用测试 ✅

```bash
$ echo "Say hello" | ode --model gpt-6.2-sol
Hello!
```

### 单元测试 ✅

```bash
$ ctest --preset dev
100% tests passed, 0 tests failed out of 2
```

## 功能特性

现在 provider 完全支持：

✅ **自动获取模型列表** - 从 API 的 `/v1/models` 端点获取  
✅ **自定义模型名称** - 支持任意模型名，包括带前缀的（如 `power/claude-opus-5-5`）  
✅ **多协议支持** - anthropic, openai-chat, openai-responses  
✅ **Fallback 机制** - API 失败时使用已知模型列表  
✅ **完全兼容** - 不影响现有功能

## 使用示例

### 1. 查看可用模型

```bash
ode models
```

### 2. 使用特定模型

```bash
ode --model gpt-6.2-sol "你的问题"
```

### 3. 配置文件

`~/.ode/settings.json`:
```json
{
  "provider": "passion",
  "model": "gpt-6.2-sol",
  "providers": {
    "passion": {
      "protocol": "openai-responses",
      "base_url": "https://api.passionapi.com/v1",
      "api_key": "sk-your-api-key-here",
      "model": "gpt-6.2-sol"
    }
  }
}
```

## 修改的文件

- `src/main.cpp` - 修复 provider_cfg 初始化顺序
- `src/provider/models.cpp` - 改进模型列表获取逻辑

## 兼容性

- ✅ 向后兼容 - 不影响现有配置
- ✅ 测试通过 - 所有单元测试正常
- ✅ 多 provider 支持 - anthropic, openai, groq, together, deepseek 等

## 注意事项

⚠️ **模型名称必须与 API 返回的完全匹配**

如果遇到 404 错误：
1. 运行 `ode models` 查看实际可用的模型
2. 更新配置文件使用正确的模型名
3. 某些 API（如 PassionAPI）的模型可能带有前缀（如 `power/`）

## 下一步

建议的改进：
- [ ] 添加交互式模型选择器
- [ ] 缓存模型列表减少 API 调用
- [ ] 显示模型的详细信息（context window, capabilities）
- [ ] 支持模型搜索和过滤
