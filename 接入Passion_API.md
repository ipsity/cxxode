# 接入 Passion API (ccswitch)

## 步骤 1：准备 API 信息

从 ccswitch 获取：
- API Base URL
- API Key
- 确认协议类型（OpenAI Responses）

## 步骤 2：配置文件

创建 `~/.cxxode/config.json`（Windows: `%USERPROFILE%\.cxxode\config.json`）：

```json
{
  "provider": {
    "type": "openai_responses",
    "base_url": "https://your-passion-api-url/v1",
    "api_key": "your-api-key",
    "model": "gpt-6.2-sol"
  },
  "permissions": {
    "mode": "default"
  }
}
```

## 步骤 3：或使用命令行配置

```bash
# 方式 1：直接创建配置
mkdir -p ~/.cxxode
cat > ~/.cxxode/config.json <<JSON
{
  "provider": {
    "type": "openai_responses",
    "base_url": "https://your-passion-api-url/v1",
    "api_key": "your-api-key",
    "model": "gpt-6.2-sol"
  }
}
JSON

# 方式 2：使用环境变量
export OPENAI_API_BASE=https://your-passion-api-url/v1
export OPENAI_API_KEY=your-passion-key
export OPENAI_MODEL=gpt-6.2-sol
```

## 步骤 4：验证配置

```bash
cd D:/02_开发项目/lay-code
./build/dev/ode.exe /model
# 应该显示：gpt-6.2-sol (openai_responses)
```

## 步骤 5：测试连接

```bash
./build/dev/ode.exe "你好，测试 Passion API 连接"
```

## 配置说明

- **type**: `openai_responses` - 使用 OpenAI Responses 协议
- **base_url**: Passion API 的基础 URL
- **api_key**: 你的 API key
- **model**: `gpt-6.2-sol` - 指定的模型

## 如果需要我帮你配置

请提供：
1. Passion API 的 Base URL
2. API Key（可以私密提供）
3. 确认模型名称：gpt-6.2-sol

我会帮你创建配置文件并测试。
