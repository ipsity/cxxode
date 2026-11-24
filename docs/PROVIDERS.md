# 模型提供者配置指南

## 概述

cxxode 支持多种 AI 模型提供者，包括 Anthropic Claude、OpenAI GPT、以及其他 OpenAI 兼容的 API。

## 内置提供者

### Anthropic (默认)

**支持的模型：**
- `opus` / `claude-opus-4-8` - 最强大的模型
- `sonnet` / `claude-sonnet-4-6` - 平衡性能（默认）
- `haiku` / `claude-haiku-4-5-20251001` - 快速高效
- `fable` / `claude-fable-5` - 轻量级模型

**配置：**
```json
{
  "provider": "anthropic",
  "model": "sonnet"
}
```

**环境变量：**
- `ANTHROPIC_API_KEY` - API 密钥（必需）
- `ANTHROPIC_BASE_URL` - 自定义 API 端点（可选）
- `ANTHROPIC_DEFAULT_SONNET_MODEL` - 覆盖默认 Sonnet 模型
- `ANTHROPIC_MODEL` - 全局模型覆盖

**定价：**
- Opus: $15/MTok input, $75/MTok output
- Sonnet: $3/MTok input, $15/MTok output
- Haiku: $0.80/MTok input, $4/MTok output

### OpenAI

**支持的模型：**
- `gpt-4o` - 多模态旗舰（默认）
- `gpt-4o-mini` - 快速且实惠
- `gpt-4-turbo` - 上一代旗舰
- `gpt-3.5-turbo` - 经济实惠

**配置：**
```json
{
  "provider": "openai",
  "model": "gpt-4o"
}
```

**环境变量：**
- `OPENAI_API_KEY` - API 密钥（必需）
- `OPENAI_BASE_URL` - 自定义 API 端点（可选）

**定价：**
- GPT-4o: $2.5/MTok input, $10/MTok output
- GPT-4o-mini: $0.15/MTok input, $0.6/MTok output

### Groq (快速推理)

**支持的模型：**
- `llama-3.3-70b-versatile` - Llama 3.3 70B（默认）
- `llama-3.1-70b-versatile` - Llama 3.1 70B
- `mixtral-8x7b-32768` - Mixtral MoE

**配置：**
```json
{
  "provider": "groq",
  "model": "llama-3.3-70b-versatile"
}
```

**环境变量：**
- `GROQ_API_KEY` - API 密钥（必需）
- `GROQ_BASE_URL` - 自定义端点（可选）

**特点：** 极快的推理速度，免费层可用

### DeepSeek (代码专用)

**支持的模型：**
- `deepseek-chat` - 通用对话（默认）
- `deepseek-coder` - 代码专用

**配置：**
```json
{
  "provider": "deepseek",
  "model": "deepseek-coder"
}
```

**环境变量：**
- `DEEPSEEK_API_KEY` - API 密钥（必需）
- `DEEPSEEK_BASE_URL` - 自定义端点（可选）

**定价：** $0.14/MTok input, $0.28/MTok output

### Together AI

**配置：**
```json
{
  "provider": "together",
  "model": "meta-llama/Meta-Llama-3.1-70B-Instruct-Turbo"
}
```

**环境变量：**
- `TOGETHER_API_KEY` - API 密钥（必需）

### Fireworks AI

**配置：**
```json
{
  "provider": "fireworks",
  "model": "accounts/fireworks/models/llama-v3p1-70b-instruct"
}
```

**环境变量：**
- `FIREWORKS_API_KEY` - API 密钥（必需）

## 列出可用模型

```bash
# 列出当前提供者的所有可用模型
ode models

# 切换提供者后列出
ode --provider openai models
```

输出示例：
```
Fetching models from openai...

Available models:

  gpt-4o  - Multimodal flagship  (128k context)
  gpt-4o-mini  - Fast and affordable  (128k context)
  gpt-4-turbo  - Previous generation  (128k context)
  gpt-3.5-turbo  - Fast and cheap  (16k context)

Use: ode --model <model-id>
```

## 自定义提供者

在 `settings.json` 中添加自定义提供者：

```json
{
  "providers": {
    "my-provider": {
      "protocol": "openai-chat",
      "base_url": "https://api.example.com/v1",
      "api_key_env": "MY_PROVIDER_API_KEY",
      "model": "my-model-name"
    }
  }
}
```

**支持的协议：**
- `anthropic` / `messages` - Anthropic Messages API
- `openai-chat` / `chat-completions` - OpenAI Chat Completions API
- `openai-responses` / `responses` - OpenAI Responses API（旧版）

## 模型别名

cxxode 提供简短的别名来快速切换常用模型：

| 别名 | 解析为 | 环境变量覆盖 |
|------|--------|--------------|
| `opus` | `claude-opus-4-8` | `ANTHROPIC_DEFAULT_OPUS_MODEL` |
| `sonnet` | `claude-sonnet-4-6` | `ANTHROPIC_DEFAULT_SONNET_MODEL` |
| `haiku` | `claude-haiku-4-5-20251001` | `ANTHROPIC_DEFAULT_HAIKU_MODEL` |
| `fable` | `claude-fable-5` | `ANTHROPIC_DEFAULT_FABLE_MODEL` |

**使用示例：**
```bash
ode --model opus "review this code"
ode --model haiku "quick question"
```

**上下文窗口后缀：**
```bash
# 请求 1M 上下文窗口（如果模型支持）
ode --model "opus[1m]" "analyze large codebase"
```

## API 密钥管理

### 方式 1：环境变量（推荐）

```bash
export ANTHROPIC_API_KEY=sk-ant-...
export OPENAI_API_KEY=sk-...
export GROQ_API_KEY=gsk_...
```

### 方式 2：Windows 凭据管理器

```bash
# 交互式输入并保存
ode login anthropic
ode login openai

# 自动从 Windows Credential Manager 读取
```

### 方式 3：settings.json（不推荐）

```json
{
  "env": {
    "ANTHROPIC_API_KEY": "sk-ant-..."
  }
}
```

**注意：** 不要将 API 密钥提交到版本控制！

## 配置优先级

从低到高：
1. 内置默认配置
2. `~/.ode/settings.json`
3. `~/.claude/settings.json`
4. `<workspace>/.ode/settings.json`
5. `<workspace>/.claude/settings.json`
6. 环境变量
7. 命令行参数

## 网关和代理

### 使用自定义端点

```json
{
  "providers": {
    "anthropic": {
      "base_url": "https://my-gateway.example.com"
    }
  }
}
```

### OpenAI 兼容网关

大多数 AI 网关都提供 OpenAI 兼容的 API：

```json
{
  "providers": {
    "gateway": {
      "protocol": "openai-chat",
      "base_url": "https://gateway.example.com/v1",
      "api_key_env": "GATEWAY_API_KEY",
      "model": "claude-3-5-sonnet-20241022"
    }
  }
}
```

**支持的网关：**
- Portkey
- LiteLLM
- Helicone
- OpenRouter
- 大多数企业 AI 网关

## 故障排查

### "Unknown provider" 错误

检查 `settings.json` 中是否正确配置了提供者：
```bash
ode --provider my-provider models
```

### API 密钥未找到

```bash
# Windows
echo %ANTHROPIC_API_KEY%

# 检查凭据管理器
ode login anthropic
```

### 连接错误

检查 `base_url` 是否正确：
```json
{
  "providers": {
    "anthropic": {
      "base_url": "https://api.anthropic.com"
    }
  }
}
```

### 查看加载的配置

启动 ode 时会显示加载的配置文件：
```
ode: Loaded ~/.claude/settings.json
ode: Loaded .claude/settings.json
```

## 示例配置

### 多提供者切换

```json
{
  "provider": "anthropic",
  "model": "sonnet",
  "providers": {
    "anthropic": {
      "api_key_env": "ANTHROPIC_API_KEY",
      "model": "claude-sonnet-4-6"
    },
    "openai": {
      "api_key_env": "OPENAI_API_KEY",
      "model": "gpt-4o"
    },
    "groq": {
      "api_key_env": "GROQ_API_KEY",
      "model": "llama-3.3-70b-versatile"
    }
  }
}
```

使用：
```bash
ode --provider openai "code review"
ode --provider groq "quick question"
```

### 企业网关配置

```json
{
  "providers": {
    "enterprise": {
      "protocol": "openai-chat",
      "base_url": "https://ai.company.com/v1",
      "api_key_env": "COMPANY_AI_KEY",
      "model": "claude-sonnet-4-6"
    }
  },
  "provider": "enterprise"
}
```

## 更多信息

- [Anthropic API 文档](https://docs.anthropic.com)
- [OpenAI API 文档](https://platform.openai.com/docs)
- [Groq 文档](https://console.groq.com/docs)
- [DeepSeek 文档](https://platform.deepseek.com/api-docs)
