# cxxode 架构

本文是项目唯一的设计依据，2026-11-25 起生效，取代 `archive/docs/` 下的全部旧文档。和代码冲突时，先讨论再改其中一方，不要让两者长期不一致。

## 1. 定位

Windows 原生终端 coding agent，C++20，单个静态链接的 `ode.exe`。

- 平台：只支持 Windows 10 1809+ / Windows 11，x64。工具链为 MSYS2 UCRT64 g++。不写、不验证 Linux/macOS 代码路径。
- 扩展：直接读 Claude Code 的现成格式（Skills、斜杠命令、Hooks、MCP、Subagents、Plugins），不自造插件接口。
- 前端：交互式 TUI、`-p` 打印模式、`--rpc` JSONL 协议，三者共用同一个内核。

可量化目标（以 release 构建为准，改动影响到时手动测一次）：

| 指标 | 目标 | 2026-11-25 实测 |
|---|---|---|
| exe 体积 | ≤ 8 MB | 6.0 MB |
| `ode --version` 冷启动 | ≤ 30 ms | 未单独测（bash 循环 10 次共 0.94 s，含进程创建） |
| 空闲内存 | ≤ 20 MB | 未测 |
| `ctest --preset dev` | < 60 s，全部离线 | 1.1 s |

## 2. 现状评估

重新规划的依据来自 2026-11-25 对 `src/` 的检查：

1. **分层失效**。`core` 依赖 `config`、`hooks`、`ui`（`core/rpc.cpp` 引用 `ui/term.hpp`）。`config` 与 `hooks` 互相引用。`commands` 引用 `app`，而 `app` 又引用 `commands`。`skills/make_tools.cpp` 依赖 `agents` 和 `mcp`。
2. **投机代码约 3700 行**。`src/innovation/`、`src/performance/`、`ui/dialog_renderer.hpp` 没有任何调用方。`SmartToolScheduler` 从未启用，`ToolAccess access_control` 没有使用者。`ui/theme.*` 和 `theme/theme.*` 是两套主题实现。
3. **生态兼容实际不可用**：
   - MCP stdio 用了 `Content-Length` 帧格式。MCP 规范规定 stdio 消息以换行分隔，现有 MCP server 无法通信。
   - Hooks 只认 `{"PreToolUse":[{"command":...}]}`。Claude Code 的格式是 `{"PreToolUse":[{"matcher":"Bash","hooks":[{"type":"command","command":...}]}]}`，所以用户现有的 hooks 会被静默跳过。stdin 字段名也和 Claude Code 不同（`tool`/`input` 对 `tool_name`/`tool_input`）。
4. **自动压缩没有实现**。上下文超过 80% 时只会提示用户手动执行 `/compact`。
5. **文档失控**。70 多份阶段报告（DAY/WEEK/PHASE/COMPLETE）互相矛盾，README 写的体积和启动数据也与实测不符。

## 3. 分层

```
app        TUI / print / rpc 前端，斜杠命令实现
ext        hooks skills commands agents plugins mcp
core       message session permission compact agent-loop
tools      Read Write Edit Bash Grep Glob
provider   anthropic openai_chat openai_responses sse
config     settings.json 解析、凭据、模型别名
ui         term render editor unicode theme markdown
platform   Win32：进程、管道子进程、WinHTTP、控制台、凭据管理器
base       cancel_token、json 别名、UTF-8 / 字符串小工具
```

`main.cpp` 是组合根：解析参数、读配置、装配各模块、选择前端。只有这里允许知道所有模块。

### 依赖规则

- 每层只依赖它下面的层，同层之间互不依赖。`ext` 内部的模块也互不引用，需要组合的地方放到 `main.cpp` 里装配。
- `core` 不认识 hooks、MCP、skills。它只认两类东西：一个 `std::vector<tool>`，和一组 `tool_hooks` 回调（pre/post/prompt/stop）。`ext/hooks` 负责把 settings 里的配置转成这组回调。
- `core` 不直接输出任何内容，所有输出都通过 `agent_events` 回调交给前端。
- `nlohmann::json` 只允许出现在 `provider`、`config`、`session`、`ext/mcp`、`ext/hooks` 和 `app/rpc` 里。`tool` 的输入是个例外，它本身就是 JSON Schema 对象，保留 `json`。
- 修改依赖关系时以 CMake 为准：每一层是一个 `STATIC` 库，`target_link_libraries` 只链接下层。违反分层就会链接失败，这样不用靠人工审查。

### 目录

```
src/main.cpp
src/base/       cancel.hpp  text.hpp(utf8/宽字符/clip)
src/platform/   process  pipe_process  http  console  credential
src/ui/         term render editor unicode theme markdown
src/config/     settings  credential  models(别名)
src/provider/   provider anthropic openai_chat openai_responses sse
src/tools/      tool files edit search bash
src/core/       message session permission agent compact
src/ext/        hooks skills commands agents plugins mcp
src/app/        interactive(拆分若干 cpp) print rpc slash_commands
tests/          与 src 同构；tests/fixtures/ 放录制的 SSE 和 MCP 会话
```

## 4. 核心循环

```
user text
  -> hooks.prompt          可阻止、可追加上下文
  -> compact_if_needed     input tokens > 窗口 × 0.8 时自动压缩
  -> provider.send         流式；只有收到终止事件的回复才算完整
  -> 对每个 tool_use：
       hooks.pre -> permission.check -> (ask) -> tool.run -> hooks.post
       连续的只读调用并行执行，其余调用串行
  -> 有 tool_use 就继续下一轮，否则结束
  -> hooks.stop            退出码 2 时把 stderr 作为新的用户消息继续
```

不变式：

- 每个 `tool_use` 都必须有对应的 `tool_result`。被取消或崩溃导致缺失时，由 `close_dangling_tool_calls` 补齐。
- Write/Edit 前检查文件的 mtime 和 size 是否与模型上次读到的一致，不一致就拒绝写入并让模型重新读取。
- 一个用户回合内被改动的文件都会备份，`/undo` 恢复的是最近一个改过文件的回合。
- session 是只追加的 JSONL 文件，崩溃最多丢失最后一行。

## 5. 扩展兼容规格

以 Claude Code 当前公开的格式为准。读取位置为 `~/.claude/`、`~/.ode/`、`<workspace>/.claude/`、`<workspace>/.ode/`，后读的覆盖先读的。

| 扩展 | 必须兼容的点 |
|---|---|
| settings.json | `permissions.allow/deny/defaultMode`、`hooks`、`mcpServers`、`env`、`model`。`.mcp.json` 也要读 |
| Hooks | `{事件: [{matcher, hooks: [{type:"command", command, timeout}]}]}`。stdin 字段：`session_id`、`transcript_path`、`cwd`、`hook_event_name`、`tool_name`、`tool_input`、`tool_response`、`prompt`。退出码 2 表示阻止，stderr 回传给模型。stdout 输出的 JSON 中要支持 `decision`/`reason`。命令通过 `bash`（找不到时用 `cmd /c`）执行 |
| MCP | stdio 传输，**换行分隔的 JSON-RPC**。支持 `initialize`、`tools/list`、`tools/call`，工具名格式为 `mcp__<server>__<tool>`。M3 再加 Streamable HTTP |
| Skills | `skills/<name>/SKILL.md`。系统提示里只放 name 和 description，正文由 `Skill` 工具按需加载 |
| 斜杠命令 | `commands/*.md`，支持 `$ARGUMENTS`、`$1..$9` 和 frontmatter 里的 `description`、`allowed-tools` |
| Subagents | `agents/*.md`，frontmatter 包含 `name`、`description`、`tools`、`model`。通过 `Task` 工具调用，子 agent 只返回最终文本 |
| Plugins | `.claude-plugin/plugin.json` 所在目录，里面包含上面几类文件。安装就是解压到 `~/.ode/plugins/<name>/` |

## 6. 不做的事

- 跨平台。代码里不保留 `#else` 的 POSIX 分支，Windows 以外的平台以后另行讨论。
- 自研插件 ABI、DLL 插件。需要原生能力的扩展写成 MCP server，靠进程隔离。
- 嵌入脚本语言（Lua 等）。重新讨论的条件是：出现 hook 和 MCP 都做不到的进程内定制需求（比如键位、状态栏、面板行为）。届时只开放给 `ui`/`app` 层使用，不进入 `core`。
- 没有调用方的"智能"模块：工具推荐、模式学习、对象池、线程池、自适应调度。真正需要时，带着测量数据再加。
- IDE 插件、云同步、多人协作。

## 7. 测试

- 只用 doctest。provider 通过录制的 SSE 夹具回放测试，MCP 用测试内启动的假 server（`ode.exe` 自身的隐藏子命令）测试，hooks 用 `cmd /c exit 2` 一类命令测试。
- 不允许访问网络。所有测试加起来必须在 60 s 内跑完。
- 每修一个 bug 都要补一条能复现它的测试。

## 8. 开箱即用

目标：推送到 remote 后，另一台没有任何本机状态的 Windows 10 1809+/11 x64 机器有两种方式可以用：

- **只用：** 下载 GitHub Release 的 zip，解压出 `ode.exe` 放到 PATH 下，执行 `ode login` 后即可使用。
- **开发：** `git clone`，装 MSYS2 UCRT64（gcc、cmake、ninja），执行 `cmake --preset dev && cmake --build --preset dev && ctest --preset dev`，全部通过。

约束：

- exe 只能依赖系统自带的 DLL：KERNEL32、ADVAPI32、SHELL32、WINHTTP，以及 Win10 起系统自带的 UCRT `api-ms-win-crt-*`。不允许依赖 libstdc++、libgcc、libwinpthread 等 MinGW DLL。CI 用 `objdump -p` 检查导入表，出现白名单以外的 DLL 就失败。
- 第三方依赖只能以单头文件形式放在 `third_party/`，构建时不联网下载。
- 源码、测试、配置里不允许出现本机路径或用户名。运行时状态（会话、命令历史、主题选择）只写到 `%USERPROFILE%\.ode\`，不写进工作区，也不进 git。
- 首次运行时没有任何配置文件也必须能启动。缺少密钥时给出一行可操作的提示，并以退出码 2 退出（2026-11-25 已用空 `USERPROFILE` 验证过）。
- Bash 工具优先使用 Git for Windows 的 bash，找不到时退回 PowerShell，所以目标机器不强制安装 Git。
- 用 CI 的 `windows-latest` 干净镜像代替"另一台机器"做验证，不搭虚拟机。

已知缺口：

- `.claude/command_history.txt` 写在工作区里，并且被 git 跟踪，会把个人输入历史推送到 remote。改为写到 `~/.ode/`，并从仓库删除。
- CI 只上传构建产物（artifact），没有打 tag 时自动生成 Release zip 的流程。
- 两个 remote 的历史不一致：`origin`（lay-code）可以直接快进推送。`cxxode` 上只有一个无关的"初始提交"，推送需要覆盖它。推送前要确定用哪个 remote。

## 9. 迁移计划

按顺序执行，每一步都是一次独立提交，并且满足 CLAUDE.md 的完成定义。

| 步骤 | 内容 | 验收 |
|---|---|---|
| R0 | 归档旧文档和安装脚本，写本文档，更新 CLAUDE.md/README | 构建和测试不受影响 |
| R1 | 删除第 2 节列出的投机代码，合并两套 theme | 行为不变，代码减少约 3700 行 |
| R2 | 建立 `base/`，按第 3 节拆成分层的静态库，消除反向依赖 | 违反分层时链接失败 |
| R3 | 把 MCP stdio 改为换行分隔，读取 `.mcp.json` | 能接上 `@modelcontextprotocol/server-filesystem` 并调用工具 |
| R4 | Hooks 改为 Claude Code 格式（matcher、字段名、JSON 输出） | 原样复制一份 Claude Code 的 hooks 配置就能生效 |
| R5 | 自动压缩 | 夹具测试：超过阈值时自动触发压缩 |
| R6 | Subagent frontmatter 中的 `tools`/`model`、斜杠命令参数 | 装一个 Claude Code 插件，它的 skill、命令、agent 都能用 |
| R7 | 补齐第 8 节的缺口：命令历史改到 `~/.ode/`，CI 加 DLL 白名单检查和空 HOME 冒烟测试，打 tag 时生成 Release zip | CI 在干净的 `windows-latest` 上全部通过，下载的 zip 解压后可直接运行 |
| R8 | 推送到 remote，打第一个 tag | 第 8 节两种方式都能走通 |

`archive/` 只读：保留历史，不参与构建，也不再更新。需要旧内容时搬到 `docs/` 并改写。
