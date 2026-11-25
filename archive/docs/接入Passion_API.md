# 接入 Passion API (ccswitch)

格式说明见 [配置第三方服务商.md](配置第三方服务商.md)。

## 1. 写配置

`~/.ode/settings.json`（Windows：`%USERPROFILE%\.ode\settings.json`）：

```json
{
  "provider": "passion",
  "model": "gpt-6.2-sol",
  "providers": {
    "passion": {
      "protocol": "openai-responses",
      "base_url": "https://api.passionapi.com",
      "api_key_env": "PASSION_API_KEY",
      "model": "gpt-6.2-sol"
    }
  }
}
```

ode 会请求 `<base_url>/responses`。如果服务商要求 `/v1` 前缀，把它写进 `base_url`。

## 2. 存密钥

二选一，密钥不要写进仓库里的任何文件：

```bash
setx PASSION_API_KEY "<你的 key>"    # 新开终端后生效
ode login passion                   # 或存入 Windows 凭据管理器
```

## 3. 验证

```bash
cd D:/02_开发项目/cxxode
./build/dev/ode.exe -p "你好，测试 Passion API 连接"
```
