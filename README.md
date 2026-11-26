# cxxode

Windows 原生终端 coding agent，C++20，单个 `ode.exe`。直接读取 Claude Code 的 Skills、斜杠命令、Hooks、MCP、Subagents 和 Plugins。

设计与路线：[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md)。

## 构建

需要 MSYS2 UCRT64 的 g++、CMake 3.24+ 和 Ninja。

```
cmake --preset release
cmake --build --preset release
```

产物是 `build/release/ode.exe`，复制到 PATH 中的任意目录即可。

## 使用

```
ode login anthropic        # 密钥存入 Windows 凭据管理器
ode                        # 在当前目录开始交互会话
ode -p "解释这个项目"       # 打印模式
ode -c                     # 继续上一次会话
ode --help
```

配置文件：`~/.claude/settings.json`、`~/.ode/settings.json`、`<项目>/.claude/settings.json`、`<项目>/.ode/settings.json`。

HTTP 请求默认使用 Windows 代理设置。需要直连特定 API 时，可在环境变量 `NO_PROXY` 中列出逗号分隔的主机名或域名，也可以在 `~/.ode/settings.json` 的 `env` 中设置它。

## Lua 扩展

`ode.exe` 内嵌 Lua 5.4。个人脚本放在 `%USERPROFILE%\.ode\init.lua`，插件脚本放在已安装插件的 `ode/init.lua`。安装和运行步骤见 [Lua 示例插件](.claude/plugins/example-plugin/README.md)。

## 历史

旧文档和安装脚本可在 `v0.3.3` 标签的 `archive/` 中查阅。V1 源码可在 `v1-final` 标签查阅，原 `legacy/` 目录也保存在 `v0.3.3` 标签中。
