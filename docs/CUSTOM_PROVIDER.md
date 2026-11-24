# 自定义服务商配置指南

## 方法 1：使用内置的兼容服务商

cxxode 已经内置了多个 OpenAI 兼容的服务商：

### Groq（免费且快速）
```bash
# 设置 API 密钥
$env:GROQ_API_KEY = "gsk-你的密钥"

# 使用
ode --provider groq "你好"
```

### DeepSeek（代码专用，便宜）
```bash
$env:DEEPSEEK_API_KEY = "sk-你的密钥"
ode --provider deepseek --model deepseek-coder "写代码"
```

### Together AI
```bash
$env:TOGETHER_API_KEY = "你的密钥"
ode --provider together "你好"
```

### OpenAI
```bash
$env:OPENAI_API_KEY = "sk-你的密钥"
ode --provider openai --model gpt-4o "你好"
```

## 方法 2：配置完全自定义的服务商

创建或编辑：`~/.ode/settings.json` 或 `.claude/settings.json`

### 示例 1：自定义 OpenAI 兼容 API
```json
{
  "providers": {
    "my-custom": {
      "protocol": "openai-chat",
      "base_url": "https://your-api.com/v1",
      "api_key_env": "MY_API_KEY",
      "model": "gpt-3.5-turbo"
    }
  }
}
```

使用：
```bash
$env:MY_API_KEY = "your-key"
ode --provider my-custom "你好"
```

### 示例 2：本地模型（如 Ollama）
```json
{
  "providers": {
    "ollama": {
      "protocol": "openai-chat",
      "base_url": "http://localhost:11434/v1",
      "api_key_env": "OLLAMA_API_KEY",
      "model": "llama2"
    }
  }
}
```

注意：Ollama 可能不需要 API key，可以设置为空字符串

### 示例 3：国内中转 API
```json
{
  "providers": {
    "proxy": {
      "protocol": "openai-chat",
      "base_url": "https://your-proxy.com/v1",
      "api_key_env": "PROXY_API_KEY",
      "model": "gpt-3.5-turbo"
    }
  }
}
```

### 示例 4：Azure OpenAI
```json
{
  "providers": {
    "azure": {
      "protocol": "openai-chat",
      "base_url": "https://your-resource.openai.azure.com/openai/deployments/your-deployment",
      "api_key_env": "AZURE_OPENAI_KEY",
      "model": "gpt-4"
    }
  }
}
```

## 支持的协议类型

- `"openai-chat"` - OpenAI Chat Completions API（最常用）
- `"openai-responses"` - OpenAI Responses API
- `"anthropic"` - Anthropic Messages API

大多数兼容 OpenAI 的服务商都用 `"openai-chat"`

## 配置文件位置优先级

1. `<项目>/.ode/settings.json` - 项目专用配置（优先级最高）
2. `<项目>/.claude/settings.json` - Claude Code 兼容
3. `~/.ode/settings.json` - 用户全局配置
4. `~/.claude/settings.json` - Claude Code 全局兼容

## 完整配置示例

```json
{
  "provider": "my-custom",
  "model": "gpt-3.5-turbo",
  "providers": {
    "my-custom": {
      "protocol": "openai-chat",
      "base_url": "https://api.example.com/v1",
      "api_key_env": "MY_API_KEY",
      "model": "gpt-3.5-turbo"
    },
    "backup": {
      "protocol": "openai-chat",
      "base_url": "https://backup-api.com/v1",
      "api_key_env": "BACKUP_KEY",
      "model": "gpt-4"
    }
  }
}
```

## 测试自定义服务商

```bash
# 1. 设置环境变量
$env:MY_API_KEY = "your-key"

# 2. 测试连接
ode --provider my-custom "你好"

# 3. 列出可用模型（如果支持）
ode --provider my-custom models

# 4. 切换模型
ode --provider my-custom --model another-model "你好"
```

## 常见问题

### Q: 如何使用不需要 API key 的本地模型？
A: 设置一个假的环境变量：
```bash
$env:LOCAL_API_KEY = "not-needed"
```
或者在代码中留空（需要修改代码）

### Q: 如何使用非 OpenAI 兼容的 API？
A: 目前只支持 OpenAI 和 Anthropic 协议。如果你的 API 使用其他协议，需要通过中转层转换。

### Q: 国内如何使用？
A: 使用国内的 OpenAI 中转服务，或者使用国内服务商如 DeepSeek：
```bash
$env:DEEPSEEK_API_KEY = "sk-..."
ode --provider deepseek "你好"
```

### Q: 如何设置默认服务商？
A: 在 settings.json 中设置：
```json
{
  "provider": "my-custom",
  "model": "my-default-model"
}
```

## 需要帮助？

告诉我你想用什么服务商，我可以帮你生成配置！

常见的：
- 🔥 Groq（免费快速）
- 🇨🇳 DeepSeek（国内，便宜）
- 🏠 Ollama（本地运行）
- 🌐 自定义中转 API
- ☁️ Azure OpenAI
- 其他...
