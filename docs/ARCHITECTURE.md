# cxxode 架构

本文是项目唯一的设计依据，自 2026-11-25 起生效。旧设计文档归档在 `v0.3.3` 标签的 `archive/docs/`。和代码冲突时，先讨论再改其中一方，避免长期不一致。

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
ext        hooks skills commands agents plugins mcp lua
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
- `nlohmann::json` 只允许出现在 `provider`、`config`、`session`、`ext/mcp`、`ext/hooks`、`ext/lua` 和 `app/rpc` 的协议边界。`tool` 的输入是个例外，它本身就是 JSON Schema 对象，保留 `json`。
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
src/ext/        hooks skills commands agents plugins mcp lua
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
- 子 agent 沿用主 agent 的工具权限规则和前后置 Hooks，由当前前端处理权限询问，文件备份归入主回合。
- session 是只追加的 JSONL 文件，崩溃最多丢失最后一行。

交互界面在终端备用屏幕上绘制。启动时把原屏幕可见的命令和输出放到对话区开头，恢复会话时接着显示会话中的消息。对话从窗口顶部向下排，填满后跟随新输出。滚轮或 Page Up / Page Down 只移动对话视窗，向上翻阅时保留当前起始行，向下翻到底后继续跟随。输入框和状态栏固定在窗口底部。打开 `/` 命令面板时，输入框上移并在下方显示候选项。

模型文本和 Bash 输出收到后逐段刷新，未换行的内容也显示。Bash 命令单独显示，已输出的行留在对话区。Write 和 Edit 成功后显示有行数上限的增删预览。工具完成行显示实际执行耗时，权限确认及调用前后的 hooks 不计入；模型发送参数时单独显示等待时间。

## 5. 扩展兼容规格

以 Claude Code 当前公开的格式为准。读取位置为 `~/.claude/`、`~/.ode/`、`<workspace>/.claude/`、`<workspace>/.ode/`。各层 Hooks 按读取顺序合并，其余配置按各自的合并规则处理，标量以后读的值为准。

| 扩展 | 必须兼容的点 |
|---|---|
| settings.json | `permissions.allow/deny/defaultMode`、`hooks`、`mcpServers`、`env`、`model`。`.mcp.json` 也要读 |
| Hooks | `{事件: [{matcher, hooks: [{type:"command", command, timeout}]}]}`。stdin 字段：`session_id`、`transcript_path`、`cwd`、`hook_event_name`、`tool_name`、`tool_input`、`tool_response`、`prompt`。退出码 2 表示阻止，stderr 回传给模型。stdout 输出的 JSON 中要支持 `decision`/`reason`。命令通过 `bash`（找不到时用 `cmd /c`）执行 |
| MCP | stdio 传输，**换行分隔的 JSON-RPC**。支持 `initialize`、`tools/list`、`tools/call`，工具名格式为 `mcp__<server>__<tool>`。M3 再加 Streamable HTTP |
| Skills | `skills/<name>/SKILL.md`。系统提示里只放 name 和 description，正文由 `Skill` 工具按需加载 |
| 斜杠命令 | `commands/*.md`，支持 `$ARGUMENTS`、`$1..$9` 和 frontmatter 里的 `description`、`allowed-tools` |
| Subagents | `agents/*.md`，frontmatter 包含 `name`、`description`、`tools`、`model`。通过 `Task` 工具调用，子 agent 只返回最终文本 |
| Plugins | `.claude-plugin/plugin.json` 所在目录，里面包含上面几类文件。安装就是解压到 `~/.ode/plugins/<name>/` |

## 6. Lua 扩展

Lua 用来做进程内扩展，覆盖两类需求：高频 hook（在 Windows 上每次都启动 Git Bash 子进程太慢）和不想另装 Node/Python 的自定义工具或 TUI 定制。Agent Core 不迁移到 Lua。

**位置**。脚本只有两处：`~/.ode/init.lua`（用户配置），以及插件目录里的 `ode/init.lua`。插件仍然使用 Claude Code 格式，`.claude-plugin/plugin.json` 不变，其他工具会忽略 `ode/` 目录。cxxode 不定义自己的插件 manifest。

**信任模型**。Lua 插件属于可信代码，安装插件这一步就是信任决定，不做 manifest 权限声明，也不做授权流程。不可信的扩展用 MCP，靠进程隔离。

**受控 API**。移除 `io`、`os`、`package.loadlib`、`debug`。这样做是为了正确性，不是为了安全：Lua 自带的 `io.open` 走 ANSI 版 `fopen`，遇到中文路径会失败；另外文件和进程操作必须经过权限引擎，`/undo` 的备份和 mtime 检查才能生效。替代接口是 `ode.fs.*`、`ode.exec`，它们经过和内置工具相同的权限检查。

**介入点**。只开放以下这些，每一个在 C++ 里都有默认行为，Lua 出错时退回默认行为并给出提示：

| 介入点 | 用途 |
|---|---|
| `ode.on("pre_tool" / "post_tool" / "prompt" / "stop", fn)` | 与 Claude Code hooks 语义相同，在进程内执行 |
| `ode.tool{ name, description, schema, access, run }` | 注册给模型调用的工具，进入统一的工具注册表 |
| `ode.command(name, fn)` | 斜杠命令 |
| `ode.ui.set_status(text)`、`ode.keymap(key, fn)` | 状态栏和键位 |

注册在 `init.lua` 中进行。Hook 收到与 shell hook 相同的字段，返回同格式的决策表或 `nil`。配置中的 shell hook 先执行，随后按 Lua 注册顺序执行。工具的 `schema` 描述 JSON 对象，`access` 为 `read`、`write` 或 `execute`。工具回调收到参数表，斜杠命令回调收到命令尾部的原始字符串，两者可返回字符串或含 `text`、`is_error`、`summary`、`detail` 的表。Lua 命令与现有命令重名时优先执行现有命令。

工具和命令回调可调用 `ode.fs.read(path[, offset, limit])`、`ode.fs.write(path, content)`、`ode.fs.edit(path, old, new[, replace_all])` 和 `ode.exec(command[, timeout_ms])`。它们通过对应内置工具运行，遵守权限、Hooks、文件时间戳和撤销机制；当前实现中，这些操作都要求存在工具或命令回调上下文。JSON 空数组用 `ode.array()` 表示，JSON 空值用 `ode.null` 表示。`ode.on` 支持 `pre_tool`、`post_tool`、`prompt`、`stop`；返回值复用 shell hook 的 `decision`、`reason` 和 `hookSpecificOutput` 约定。`ode.command(name, fn)` 注册斜杠命令，命令回调在 worker 上执行，UI 线程不等待 Lua。

**执行模型**：

- 单独一个 Lua 线程持有唯一的 VM，所有调用都以消息的形式投递给它。
- API 一律是同步风格（`local r = ode.exec("git status")`），在 Lua 线程上阻塞执行。以后如果需要并发，在 C++ 侧改用 `lua_yield`/`lua_resume`，脚本不用改。不做回调式的异步 API。
- 渲染路径上 C++ 永远不同步等待 Lua。UI 相关的状态由 Lua 推送给 C++，C++ 不按帧调用 Lua。
- 用 `lua_sethook` 做指令计数，用来响应取消和限制单次调用的执行时间；用自定义 `lua_Alloc` 限制内存。
- 每个脚本使用独立 `_ENV`，通过受限全局表继承标准库；插件脚本获得浅拷贝的 `ode` 表和 `ode.plugin={name, version, path}`。`require(name)` 只从当前脚本目录查找点号映射到目录分隔符的 `.lua` 文件，按脚本缓存，不加载 C 模块或字节码。
- Lua 虚拟机只在至少有一个脚本文件时创建；没有脚本时不创建线程或 VM。脚本加载失败只回滚该脚本注册的扩展并记录警告。
- 每次脚本调用最多 400 万条 Lua 指令、2 秒 Lua 执行时间、8 MiB VM 堆；进入内置工具或权限询问时暂停 Lua 时间计数。取消或关闭宿主会通过指令 hook 中止执行。
- 受限环境只打开 base、table、string、math、utf8；移除 `dofile`、`loadfile`、`pcall`、`xpcall`、`setmetatable`。脚本环境提供的 `load` 强制文本模式并默认继承当前脚本环境；`print` 将参数以制表符分隔后写入 ode 的 stderr 日志。
- Lua 源码按 C++ 编译，`lua_error` 走 C++ 异常，绑定代码里的 RAII 对象能正常析构。
- 边界上的数据转换（JSON 和 Lua table 互转）只在 `ext/lua` 里做。

**生命周期**。先加载 `~/.ode/init.lua`，再按路径顺序加载已发现的 Claude Code 格式插件下的 `ode/init.lua`。插件的 skills 继续按插件发现规则加载。没有 Lua 脚本时跳过 VM 创建。`/reload` 销毁 VM、清空 Lua 状态栏文字，并重新发现和加载 Lua 脚本。版本用 `ode.api_version` 整数表示，不兼容的改动就加 1。

空闲时，`ode.keymap("Ctrl+G", fn)` 可绑定未占用的 Ctrl 字母组合以及 F1–F12；Ctrl+C/J 和编辑器现有的 Ctrl+A/D/E/K/L/O/T/U/W 保留。注册冲突会作为脚本加载警告处理。按键回调在 worker 上运行，UI 线程不等待；回调收到键名，返回 `false` 或发生错误时交还给原键位处理。权限询问和其他弹窗拥有键盘优先权。`/help` 列出已注册的 Lua 键位。`ode.ui.set_status("")` 清空状态栏文字。

**预算**。exe 体积增加不超过 400 KB。没有脚本时，启动时间和内存与纯 C++ 版本相同。

## 7. 不做的事

- 跨平台。代码里不保留 `#else` 的 POSIX 分支，Windows 以外的平台以后另行讨论。
- 自研插件 ABI、DLL 插件。需要原生能力的扩展写成 MCP server，靠进程隔离。
- 没有调用方的"智能"模块：工具推荐、模式学习、对象池、线程池、自适应调度。真正需要时，带着测量数据再加。
- IDE 插件、云同步、多人协作。

## 8. 测试

- 只用 doctest。provider 通过录制的 SSE 夹具回放测试，MCP 用测试内启动的假 server（`ode.exe` 自身的隐藏子命令）测试，hooks 用 `cmd /c exit 2` 一类命令测试。
- 不允许访问网络。所有测试加起来必须在 60 s 内跑完。
- 每修一个 bug 都要补一条能复现它的测试。

## 9. 开箱即用

目标：推送到 remote 后，另一台没有任何本机状态的 Windows 10 1809+/11 x64 机器有两种方式可以用：

- **只用：** 下载 GitHub Release 的 zip，解压出 `ode.exe` 放到 PATH 下，执行 `ode login` 后即可使用。
- **开发：** `git clone`，装 MSYS2 UCRT64（gcc、cmake、ninja），执行 `cmake --preset dev && cmake --build --preset dev && ctest --preset dev`，全部通过。

约束：

- exe 只能依赖系统自带的 DLL：KERNEL32、ADVAPI32、SHELL32、WINHTTP，以及 Win10 起系统自带的 UCRT `api-ms-win-crt-*`。不允许依赖 libstdc++、libgcc、libwinpthread 等 MinGW DLL。CI 用 `objdump -p` 检查导入表，出现白名单以外的 DLL 就失败。
- 第三方依赖放在 `third_party/`，随仓库提交，构建时不联网下载。除 `third_party/lua/`（Lua 5.4 源码）外只允许单头文件。
- 源码、测试、配置里不允许出现本机路径或用户名。运行时状态（会话、命令历史、主题选择、权限档位）只写到 `%USERPROFILE%\.ode\`，不写进工作区，也不进 git。各工作区共用 `command_history.txt`。
- `/mode` 和 `Shift+Tab` 切换权限档位时保存选择。交互模式启动时按 `--permission`、上次选择、`permissions.defaultMode` 的顺序决定档位。打印和 RPC 模式沿用各自的启动规则。
- 首次运行时没有任何配置文件也必须能启动。缺少密钥时给出一行可操作的提示，并以退出码 2 退出（2026-11-25 已用空 `USERPROFILE` 验证过）。
- `ode login [provider]` 和 `ode logout [provider]` 管理 Windows 凭据管理器中的密钥。交互界面的 `/logout` 同时结束当前 provider 的登录状态。启动和 `/login` 按原优先级加载配置文件、环境变量和 Windows 凭据。推荐在 `providers.<name>.model` 设置默认模型。顶层 `model` 归属同一配置层选中的 provider，也可用 `model_provider` 指明归属。命令行 `--model` 只覆盖当次启动。
- Bash 工具优先使用 Git for Windows 的 bash，找不到时退回 PowerShell，所以目标机器不强制安装 Git。
- 用 CI 的 `windows-latest` 干净镜像代替"另一台机器"做验证，不搭虚拟机。
- `v*` tag 的构建通过后，GitHub Actions 发布仅含 `ode.exe` 的 `ode-<tag>-windows-x64.zip`。

已知缺口：

- 两个 remote 的历史不一致：`origin`（lay-code）可以直接快进推送。`cxxode` 上只有一个无关的"初始提交"，推送需要覆盖它。推送前要确定用哪个 remote。

## 10. 迁移计划

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
| R7 | 补齐第 9 节的缺口：命令历史改到 `~/.ode/`，CI 加 DLL 白名单检查和空 HOME 冒烟测试，打 tag 时生成 Release zip | CI 在干净的 `windows-latest` 上全部通过，下载的 zip 解压后可直接运行 |
| R8 | 推送到 remote，打第一个 tag | 第 9 节两种方式都能走通 |
| L1 | `third_party/lua/` 引入 Lua 5.4 源码并按 C++ 编译；建立 `ext/lua` 宿主：Lua 线程、移除标准库、指令和内存限制、`/reload` | 没有脚本时 VM 不创建；死循环脚本会被中止；exe 体积增加 ≤ 400 KB |
| L2 | 实现 `ode.fs`、`ode.exec`、`ode.on`、`ode.tool`、`ode.command`，全部接入权限引擎和统一的工具注册表 | 用 Lua 写的 pre_tool hook 能阻止 Bash 调用；Lua 工具可以被模型调用 |
| L3 | 读取插件里的 `ode/init.lua`，实现 `ode.ui.set_status`、`ode.keymap` | 装一个带 `ode/init.lua` 的 Claude Code 插件，它的 skill 和 Lua 部分都能用 |

R 系列先完成并发布纯 C++ 版本，L 系列在其后。这样 Lua 宿主可以直接放在 R2 拆好的 `ext` 层里，移植问题也不会和 Lua 的问题混在一起。

本地 `archive/` 由 Git 忽略，只作历史参考。也可从 `v0.3.3` 标签查阅旧版归档。需要沿用的内容应在 `docs/` 中按当前设计改写。
