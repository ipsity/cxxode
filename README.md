# cxxode

**ode** 是面向 Windows 10 1809+ / Windows 11 x64 的原生终端 coding agent。它使用 C++20 构建为单个 `ode.exe`，支持交互式终端、打印模式和 JSONL RPC，并可读取 Claude Code 格式的 Skills、斜杠命令、Hooks、MCP、Subagents 与 Plugins。

- [下载最新 Windows x64 版本](https://github.com/ipsity/lay-code/releases/latest)
- [设计与系统行为](docs/ARCHITECTURE.md)

## 快速开始

1. 从 [GitHub Releases](https://github.com/ipsity/lay-code/releases/latest) 下载 `ode-<version>-windows-x64.zip`，解压并将 `ode.exe` 放入 PATH。
2. 在终端运行 `ode login`，按提示保存 API 密钥。
3. 进入项目目录运行 `ode`。

密钥保存在 `%USERPROFILE%\.ode\settings.json`。该文件以明文保存，请勿将其提交到版本控制。`ode logout [provider]` 可删除密钥；旧版 ode 保存在 Windows 凭据管理器中的密钥可通过 `ode login [provider] --import-windows-credential` 显式导入。环境变量中的 provider 密钥不会用于连接解析。

## 使用方式

```text
ode                         当前目录启动交互会话
ode "检查这个项目的错误处理"  以首条提示启动交互会话
ode -c                      继续当前工作区最近的会话
ode -p "解释这个项目"        打印模式；回答写入 stdout，工具状态写入 stderr
ode --rpc                   通过 stdin/stdout 使用 JSONL RPC
ode models                  列出当前 provider 可用模型
ode --help                  查看全部启动参数
```

常用参数：

- `-C DIR`：指定工作区目录。
- `--provider NAME`：选择 provider。
- `--model NAME`：覆盖本次启动的模型，也支持 `opus`、`sonnet`、`haiku`、`fable` 别名。
- `--permission plan|default|yolo`：选择本次启动的权限档位。
- `--no-color` 或 `NO_COLOR=1`：关闭 ANSI 颜色。

交互模式常用命令：`/help`、`/model`、`/mode`、`/theme`、`/resume`、`/compact`、`/undo`、`/login`、`/logout`、`/cost`、`/clear`、`/exit`。`Shift+Tab` 可切换权限档位。工具执行前仍会按当前权限规则验证和询问；`/undo` 可恢复最近一轮修改的文件。

## Provider 配置

内置 provider 为 Anthropic、OpenAI 和 DeepSeek。运行 `/login`，选择「使用 API Key」及要连接的服务商。使用 `/login <provider>` 或 `ode login [provider]` 可直接录入或更新密钥。

选择「第三方服务商」→「添加第三方服务商」，填写服务商 ID、协议、Base URL 和 API Key。ode 会查询模型列表供你选择默认模型，也可手动输入模型 ID。Base URL 可包含服务商要求的路径，例如 `https://gateway.example.com/api/v1`。

provider、模型和连接设置位于用户级 `%USERPROFILE%\.ode\settings.json`。例如：

```json
{
  "provider": "anthropic",
  "providers": {
    "anthropic": {
      "model": "claude-sonnet-4-6"
    }
  }
}
```

自定义服务可在用户级设置中定义协议、地址和认证方式：

```json
{
  "provider": "my-gateway",
  "providers": {
    "my-gateway": {
      "protocol": "openai-chat",
      "base_url": "https://api.example.com/v1",
      "auth": "bearer",
      "model": "my-model"
    }
  }
}
```

`protocol` 支持 `anthropic`、`openai-chat` 和 `openai-responses`。`auth` 支持 `api-key`、`bearer`、`none`；密钥也可由 `ode login my-gateway` 写入。工作区 `.ode/settings.json` 可选择已在用户配置中定义的 provider 和模型，不用于保存密钥。顶层 `model` 可覆盖当前配置层所选 provider 的模型；用 `model_provider` 可指定它所属的 provider。

HTTP 请求默认使用 Windows 代理设置。需要对特定主机直连时，可在 `NO_PROXY` 或用户设置的 `env` 中指定主机名/域名列表。

## 权限与文件安全

权限档位为 `plan`、`default` 和 `yolo`。工具调用仍经过输入校验、Hooks 和权限判断；流式接收中的工具参数只用于显示接收进度，**只有完整参数解析并授权后才会执行**。

相对路径按工作区解析。`C:/...` 和 `C:\...` 是 Windows 绝对路径；Git Bash 的 `/c/...`、`/d/...` 会转换为对应盘符路径；`c/...` 是工作区内的相对目录。Write/Edit 会检查模型读取文件后的变化，并在写入前保留可供 `/undo` 使用的备份。

## Claude Code 扩展

**ode** 可从用户级和工作区的 `.claude`、`.ode` 目录读取兼容配置，包括 Skills、斜杠命令、Hooks、MCP、Subagents 和 Plugins。MCP stdio 使用换行分隔的 JSON-RPC。Skill 仅加载指令文本；其中请求的 Bash、Write、Edit、MCP 等工具仍分别经过权限检查。

Lua 5.4 扩展位于 `%USERPROFILE%\.ode\init.lua`，插件可提供 `ode/init.lua`。Lua 插件是进程内可信代码；不可信扩展应使用 MCP 隔离。

## 从源码构建

需要 MSYS2 UCRT64、g++、CMake 3.24+ 和 Ninja。依赖已随仓库提供，构建和测试不需要联网：

```text
cmake --preset dev
cmake --build --preset dev
ctest --preset dev
```

发布构建使用 `release` preset：

```text
cmake --preset release
cmake --build --preset release
```

产物为 `build/release/ode.exe`。推送 `v*` tag 后，Windows CI 会运行发布构建和离线测试，并创建只含 `ode.exe` 的 Windows x64 ZIP Release。

## 历史版本

旧文档和安装脚本可在 `v0.3.3` 标签的 `archive/` 中查阅。V1 源码可在 `v1-final` 标签查阅；原 `legacy/` 目录也保存在 `v0.3.3` 标签中。
