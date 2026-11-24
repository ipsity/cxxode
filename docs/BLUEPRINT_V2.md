# cxxode V2 蓝图

2026-11-24 · 取代 `docs/00_MASTER.md` 及 `docs/modules/` 的方向性内容（旧文档保留在原处，未改动）

---

## 0. 结论

1. **方向换掉，代码回收**。现在的 Lay Code 是一个"可审计证据系统"，而你要的是"顺手的 coding agent"。在旧框架里逐项放宽（上一版规划的做法）改不掉根上的问题。V2 按 pi 的最小内核重建 agent loop，从旧代码回收网络、SSE、进程、Unicode 这几块写得好的底层模块。
2. **定位**：Windows 优先的原生终端 coding agent。启动 <20 ms、空闲内存 <15 MB、单个 exe。内核只有 6 个工具，扩展全部走 Claude Code 生态的现成格式（Skills、斜杠命令、MCP、Hooks、Plugins、Subagents），不发明自己的插件 ABI。
3. **开发流程是最大的减速源，先改流程再写代码**。完成定义收缩为"编译 + 60 秒内的自动测试 + 你自己用一次"。VM、讲述人、多窗口尺寸人工矩阵、RC 编号、验收 JSON 全部停止。
4. **磁盘 17 GB 里 16 GB 是虚拟机镜像**。源码约 1.7 MB，git 库 3.1 MB，比 pi 小得多。清理后项目目录可降到几十 MB。

---

## 1. 现状评估

### 1.1 数据

| 项 | 数值 | 说明 |
| --- | --- | --- |
| 项目目录总大小 | 17 GB | |
| `build/p4-clean-windows/` | 16 GB | `kvm/system.qcow2` 11 GB + `windows11-ltsc-eval.iso` 4.8 GB |
| `out/` | 506 MB, 563 项 | 历次 RC 便携包 |
| `build/` 其余 | 约 400 MB, 235 个子目录 | 每个阶段一份构建目录 |
| `.git` | 3.1 MB | `build/`、`out/` 已被 gitignore，没进库 |
| 产品代码 `src/` + `app/` | 约 12,000 行 | 其中 1,940 行是 Unicode 数据表 |
| 验证代码 `bench/` + `tests/` | 约 7,900 行 Python + 契约 C++ | `bench/` 49 个文件，33 个是验收 JSON 或手册 |
| 文档 | 276 KB | 一个 0.1 版本的产品 |
| 提交 | 54 次 / 3 天，+34,540 行 | 同期 RC 包出到 rc29 |
| 未提交改动 | 32 个修改 + 17 个新增 | P24–P28 工作，含 534 行 Windows UIA 原生读屏窗口 |

pi 的 150 MB 大部分是 `node_modules`。按源码比，Lay Code 很小。"项目大"完全来自构建产物和虚拟机。

### 1.2 写得好、值得回收的部分

- **启动性能**：P95 14.99 ms 到提示符，空闲私有内存 1.12 MiB（`bench/windows-p0.json`）。C++ 的价值在这里真实体现了。
- **流式协议层**：`src/model/provider_http.cpp` 的 libcurl multi + SSE 逐字节解码、429 重试、`curl_multi_wakeup` 取消；`messages_codec.cpp` 覆盖 Anthropic Messages、OpenAI Responses、Chat Completions 三种协议。
- **进程管理**：`src/platform/process_windows.cpp` 用 Job 对象回收整棵进程树，超时和 Ctrl+C 清理可靠。
- **Unicode**：`app/tui_unicode*` 的 Unicode 17 字素切分和东亚宽度，中文光标移动和删除需要它。
- **凭据**：`provider_credentials.cpp` 接 Windows 凭据管理器。
- **一条真正重要的安全原则**："流没有正常结束就不派发工具调用"。半截 JSON 不执行。

### 1.3 拖累产品的设计

| 设计 | 后果 |
| --- | --- |
| `--write FILE` 精确授权，最多 3 个文件 | 模型发现要改第 4 个文件就做不下去，只能退出重开 |
| `run` 工具没有 shell，`--allow-run` 才可用 | `git diff \| head`、管道、重定向都不能用 |
| `read` 单次最多 400 行，`search` 只支持字面量 | 模型在大文件和模糊查找上要多绕好几轮 |
| `edit` 必须携带 `version` 令牌，`new_text` ≤ 8 KB | 多一次往返，大改动被拆碎 |
| 每任务 15 分钟、请求数、工具数预算 | 长任务被硬切 |
| 会话格式 1→5 三天迁移了 4 次 | 每加一个功能就动 SQLite schema |
| 引用历史决定 Ctrl+Q/Ctrl+G、`--quote-task`、`--current-decision-task` | 用户要理解内部任务编号才能用 |
| 补充每任务只能一条、F3 双击结清、F5 继续、F6 补充、F7–F10 导航 | 快捷键靠功能键堆出来，笔记本要按 Fn |
| 全屏 TUI 自绘历史、翻页、证据面板 | 终端原生的滚动、选择、复制、搜索全部失效，只好再写一套 |
| CTest 专用结果解析 | 只对 CMake 项目有效 |

这些设计单独看都有理由，合起来让 agent 变成"每一步都要出示证据"的流程机器。

### 1.4 为什么推进慢

产品慢和开发慢是同一个原因：**把"证明正确"放在了"能用"前面**。

- 每个阶段（P0–P28）都要求出一份验收 JSON、一份人工手册、一个 RC 包。28 个阶段产出 29 个 RC，用户能感知的能力只增加了"能改 3 个文件、能跑命令"。
- 验收矩阵在叠加：Windows 净机 VM、WSL、ConPTY、讲述人、120×32/80×24/48×16 三种窗口、`--mono`、`--ascii`、微软拼音、原生 UIA 窗口。每加一个 UI 细节，就要乘一遍矩阵。
- 全屏自绘 TUI 是矩阵膨胀的源头。自绘了滚动和布局，才需要验证每种窗口尺寸；绕过了终端文本流，读屏器读不到，才需要讲述人测试和一个 534 行的 UIA 原生窗口。
- 文档声称"绝对权威"、决策"不再重新讨论"。执行的 agent 照着做，自然会把每条规则都落成检查。agent 很拖沓，是因为指令本身要求它拖沓。

所以 V2 的第一条原则不是某个技术选择，而是：**默认相信 git 和用户，只在不可逆的地方设闸**。

---

## 2. 定位

**一句话**：一个用 C++ 写的、像 pi 一样小、像 Claude Code 一样兼容生态的终端 coding agent，Windows 上体验最好。

| 对比 | Claude Code | pi | Lay Code V2 |
| --- | --- | --- | --- |
| 运行时 | Node | Node | 原生单 exe，无运行时 |
| 启动 | 数百 ms | 数百 ms | < 20 ms |
| 空闲内存 | 数百 MB | 约 100 MB 级 | < 15 MB |
| 内置工具 | 多 | 4 个 | 6 个 |
| 扩展 | Skills/MCP/Hooks/Plugins | TS 扩展/Skills | 直接读 Claude Code 的格式 |
| Windows | 依赖 Git Bash | 一般 | 首要平台，中文输入法和宽字符正确 |

**差异点只有三个，其余向主流看齐**：

1. **快**。启动、按键回显、流式渲染都在毫秒级，长会话不卡。
2. **小而可读**。内核代码量控制在 1 万行以内，一个人能读完。
3. **零学习成本的兼容**。已有的 `CLAUDE.md`、`AGENTS.md`、`.claude/skills`、`.claude/commands`、`.mcp.json` 放进来就能用。快捷键沿用 Claude Code 的习惯。

**明确不做**：IDE 插件、云端、多人协作、自研插件 ABI、自研脚本语言、可审计证据链、读屏专用窗口。

**保留的唯一硬原则**（来自 V1，确实有价值）：

- 流没有正常结束，不派发任何工具调用。
- 已派发但结果未知的 shell 命令，恢复时不自动重跑。
- 每轮编辑前备份原文件，`/undo` 可回退。

---

## 3. 系统架构

### 3.1 分层

```
┌──────────────────────────────────────────────┐
│ 前端        tui（内联渲染） │ print（-p 无头） │ rpc（JSONL stdio）│
├──────────────────────────────────────────────┤
│ 扩展层      skills · commands · hooks · mcp · subagents · plugins │
├──────────────────────────────────────────────┤
│ 内核        loop · context · permission · session(jsonl)        │
├──────────────────────────────────────────────┤
│ 适配        provider(anthropic/openai-responses/openai-chat)     │
│             tools(read/write/edit/bash/grep/glob)               │
├──────────────────────────────────────────────┤
│ 平台        http(libcurl) · process(Job/ConPTY) · fs · unicode   │
└──────────────────────────────────────────────┘
```

规则：上层依赖下层，同层互不依赖。三个前端共用同一个内核，`print` 和 `rpc` 让 Lay Code 可以被脚本、编辑器、其他 agent 调用，这也是"兼容生态"的另一半。

### 3.2 Agent loop

整个循环约 300 行，伪代码：

```
loop:
  req = context.build(messages, tools, memory, skills_index)
  stream = provider.send(req)              // 实时渲染文本增量
  reply = stream.collect()
  if !reply.complete: report error; break  // 半截回复零派发
  messages.append(reply)
  if reply.tool_calls.empty: break
  results = parallel_for(reply.tool_calls,  // 只读工具并行
              call -> hooks.pre(call) -> permission.check(call) -> tool.run(call) -> hooks.post(call))
  messages.append(results)
  if user_interrupt: break
  if steering_queue.nonempty: messages.append(steering_queue.drain())
```

要点：

- **没有任务预算、时长上限、请求计数**。停止条件只有：模型不再调用工具、用户按 Esc、出错。成本显示在状态栏，由用户自己决定。
- **插话（steering）**：模型工作时，用户输入的内容排进队列，在下一次请求前送进上下文。不限次数，不需要切模式。
- **只读工具并行**：`read`、`grep`、`glob` 同一轮内并行执行。写类工具串行。
- **上下文压缩**：接近窗口上限时，让模型把旧轮次总结成一段摘要（同 Claude Code 的 `/compact`），完整记录仍在会话文件里。

### 3.3 工具（内核只有 6 个）

| 工具 | 参数 | 说明 |
| --- | --- | --- |
| `read` | path, offset?, limit? | 默认 2000 行，支持图片 |
| `write` | path, content | 新建或整体覆盖 |
| `edit` | path, old, new, replace_all? | 精确替换，不要 version 令牌，宿主用文件 mtime+hash 检查外部修改 |
| `bash` | command, timeout?, background? | Windows 上优先 Git Bash，没有就用 PowerShell。Job 对象回收进程树 |
| `grep` | pattern, path?, glob? | 内置 ripgrep 式正则搜索，PATH 上有 `rg` 就直接调用 |
| `glob` | pattern, path? | 按修改时间排序 |

工具名和参数名与 Claude Code 对齐，这样现有 skill 文本里"用 Grep 搜索"之类的说法无需改动就能生效。其余能力（web fetch、todo、子 agent）由扩展层提供，不进内核。

### 3.4 权限

三档，`Shift+Tab` 循环切换，状态栏显示：

| 档位 | read/grep/glob | write/edit | bash |
| --- | --- | --- | --- |
| **plan**（只读） | 放行 | 拒绝 | 拒绝 |
| **default** | 放行 | 放行，自动备份 | 白名单放行，其余单键确认 |
| **yolo** | 放行 | 放行 | 放行 |

- 不需要预先声明可写文件。工作区外的写入单独确认。
- bash 白名单沿用 Claude Code 的 `permissions.allow` 规则语法，比如 `Bash(git status:*)`、`Bash(cmake --build:*)`。确认时按 `a` 表示"本会话以后都放行这个前缀"。
- 确认就是一行：`$ cmake --build build    ⏎ 运行  esc 拒绝  a 总是`。
- `edit/write` 执行前，原文件复制到 `~/.lay/backup/<session>/<turn>/`。`/undo` 回退最近一轮的全部文件改动。工作区是 git 仓库时，状态栏显示未提交文件数。

### 3.5 会话存储：JSONL 取代 SQLite

- 每个会话一个 `~/.lay/sessions/<project-hash>/<id>.jsonl`，一行一条消息，只追加。
- 进程崩溃最多丢最后一行，读取时丢弃不完整的尾行即可恢复。这已满足 coding agent 的恢复需求，不需要事务。
- 格式变化靠每行的 `"v"` 字段兼容读取，不做迁移。
- `/resume` 列出最近会话，`--continue` 直接接上一次。
- 去掉 SQLite 依赖，构建和打包都变简单。旧会话不迁移（V1 用户只有你一人）。

### 3.6 上下文与记忆

启动时按顺序加载，与 Claude Code 一致：

1. `~/.lay/LAY.md`（用户级），同时识别 `~/.claude/CLAUDE.md`
2. 从工作区根往下：`CLAUDE.md`、`AGENTS.md`、`LAY.md`
3. `@path` 引用：草稿里输入 `@` 弹出文件补全，选中的文件内容直接附进消息

系统提示控制在 1,500 token 左右，内容是：工具用法、"先做再说，少问"、"不要编造测试结果"。不再写引用格式、重读规则、证据要求。

### 3.7 扩展层：直接兼容 Claude Code 生态

原则：**读别人的格式，不定义自己的格式**。搜索路径同时包含 `.lay/` 和 `.claude/`，用户已有的配置零改动可用。

| 能力 | 格式 | 实现方式 | 阶段 |
| --- | --- | --- | --- |
| Skills | `SKILL.md`（YAML 头 + Markdown） | 启动时只把 name+description 放进系统提示，模型调用 `skill` 工具时再加载正文。Agent Skills 已是多家工具共用的开放标准 | M2 |
| 斜杠命令 | `.claude/commands/*.md` | 输入 `/name args`，展开 `$ARGUMENTS` 后作为用户消息发送 | M2 |
| MCP | `.mcp.json` / `mcpServers` | 实现 MCP 客户端：stdio 传输先做，Streamable HTTP 后做。工具名映射为 `mcp__server__tool` | M3 |
| Hooks | `settings.json` 里的 `hooks` | `PreToolUse`、`PostToolUse`、`UserPromptSubmit`、`Stop`，子进程执行，stdin 传 JSON，退出码 2 表示阻止 | M3 |
| Subagents | `.claude/agents/*.md` | 内核里的 `task` 工具：开一个独立上下文的子 loop，只返回最终文本 | M4 |
| Plugins | `.claude-plugin/plugin.json` | 插件就是上面几类文件的打包目录。实现"安装 = 解压到 `~/.lay/plugins/`"，再按 marketplace.json 拉取 | M4 |

为什么不做 C++ 原生插件（DLL）：ABI 不稳定、跨编译器不兼容、崩溃会拖垮宿主。需要原生性能的扩展可以写成 MCP server，进程隔离，用什么语言都行。

### 3.8 性能预算

这些数字写进 `bench/`，作为唯一的性能门槛，每次发版跑一次：

| 指标 | 目标 |
| --- | --- |
| 冷启动到可输入 | < 20 ms（MCP server 异步连接，不阻塞启动） |
| 空闲内存 | < 15 MB |
| 按键到屏幕 | < 5 ms |
| 流式文本到屏幕 | < 16 ms，按帧合并 |
| 1,000 轮会话恢复 | < 100 ms |
| exe 大小 | < 8 MB（静态链接 libcurl + Schannel，不带 OpenSSL） |

实现手段：单线程事件循环 + 一个 I/O 线程；渲染只重绘变化的行；会话文件 mmap 读取；JSON 只在协议边界解析。

### 3.9 依赖

| 依赖 | 用途 | 去留 |
| --- | --- | --- |
| libcurl | HTTP/SSE | 保留，Windows 用 Schannel |
| nlohmann-json | 协议 | 保留，后续若性能不够再换 simdjson 或 glaze |
| SQLite | 会话 | **移除**，改 JSONL |
| OpenSSL | Linux TLS | 仅 Linux |
| 正则 | grep 工具 | 先用 PATH 上的 `rg`，没有就退回 `std::regex` 简单实现 |

---

## 4. UI 设计

### 4.1 核心决定：内联渲染，不用全屏

V1 使用备用屏幕全屏自绘。V2 改成 Claude Code、pi 的做法：**对话直接打印进终端的普通滚动区，只有底部输入框和状态栏是活动区域**。

好处：

- 终端原生的滚动、鼠标选择、复制、Ctrl+F 搜索全部可用，不用自己写历史翻页、证据面板、F7–F10 导航。
- 已完成的内容是普通文本，读屏器可以直接读。不再需要 UIA 原生窗口，不再需要讲述人测试矩阵。
- 窗口尺寸变化只影响底部几行，不需要在 3 种尺寸下逐一人工验收。

代价：已打印的内容不能再折叠。解决办法是工具输出默认只打印摘要行，`Ctrl+O` 在活动区展开最近一条的完整输出（同 Claude Code 的 transcript 模式）。

### 4.2 版面

```
 ● 读 src/parser.cpp  1–180
 ● 搜 "empty_config"  3 处
 ● 改 src/parser.cpp  +4 −1
   │ 42 - if (text.empty()) return reject;
   │ 42 + if (text.empty()) return Config{};
 ● 跑 ctest --test-dir build -R parser   ✓ 12 通过  0.8s

 空文件现在返回默认配置，parser 的 12 个测试通过。

╭──────────────────────────────────────────────────────────╮
│ › 顺便把 README 里的说明也改一下▌                         │
╰──────────────────────────────────────────────────────────╯
  lay-code  D:/02_开发项目/lay-code  main +2      default · opus · 18k/200k · ¥0.21
```

### 4.3 视觉规则

只有三条规则，所有元素都由它们推出：

1. **一个强调色**。工具圆点和焦点用青色 `#00d4ff`，失败用红色，其余全部是终端默认前景色和一个灰色。不用背景色，沿用终端主题。
2. **三级亮度**。已完成的工具行是灰色，模型回答是正常色，需要你决定的那一行是强调色加粗。同一时刻最多一个强调行。
3. **一个符号**。`●` 表示工具调用，颜色区分状态（进行中闪烁、完成灰、失败红）。diff 用 `│` 缩进，最多显示 8 行，超出部分写 `… 还有 23 行，Ctrl+O 展开`。

其余约定：

- 不画边框，只有输入框有圆角框。
- 中文文案，动词开头，4 个字以内：读、搜、改、写、跑、问。
- `NO_COLOR` 环境变量或 `--mono` 时去掉颜色，符号和文字已足以区分状态。
- 状态栏左侧：品牌、路径（过长时省略中间）、git 分支和未提交数。右侧：权限档、模型、上下文用量、本会话花费。忙碌时右侧显示 `⠋ 思考中 12s  esc 中断`。

### 4.4 命令面板

行首输入 `/` 时，在输入框上方弹出浮层，模糊匹配，最多显示 8 项：

```
  /compact   压缩上下文
  /model     切换模型
  /resume    恢复会话
  /undo      撤销上一轮改动
  /review    （skill）审查当前改动
```

内置命令、用户 commands、skills 在同一列表里，skill 后面标 `（skill）`。`@` 触发文件补全，交互相同。

---

## 5. 操作规范

### 5.1 快捷键

原则：只用 Windows Terminal、conhost、VS Code 终端都能可靠传递的组合键；不用功能键；与 Claude Code 保持一致，避免肌肉记忆冲突。

| 键 | 空闲时 | 模型工作时 | 有待确认命令时 |
| --- | --- | --- | --- |
| `Enter` | 发送 | 排队为插话 | 运行 |
| `Ctrl+J` / `\`+`Enter` | 换行 | 换行 | — |
| `Esc` | 清空草稿；草稿为空时无操作 | 中断当前轮 | 拒绝 |
| `Esc Esc` | 回到上一条消息编辑（分叉） | — | — |
| `a` | — | — | 总是允许此前缀 |
| `Ctrl+C` | 清空草稿；连按两次退出 | 中断当前轮 | 拒绝并中断 |
| `Ctrl+D` | 草稿为空时退出 | — | — |
| `Shift+Tab` | 切换权限档 | 切换权限档 | — |
| `Ctrl+O` | 展开/收起最近工具输出 | 同左 | 展开命令详情 |
| `Ctrl+L` | 重绘屏幕 | 同左 | 同左 |
| `↑` / `↓` | 草稿为空或光标在首尾行时翻输入历史 | 同左 | — |
| `Tab` | 补全 `/` 和 `@` | — | — |
| `Ctrl+V` | 粘贴，图片则附加图片 | 同左 | — |

一共 12 个键，全部列在 `/help`。输入区不显示任何提示文字。

规则：

- **Esc 是唯一的"停"**，Ctrl+C 是终端用户的本能所以同义保留。
- 中断后已执行的工具结果保留，模型下一轮能看到。
- 不需要"补充模式""结清任务""继续任务"等概念。中断后直接输入下一句话即可。

### 5.2 运行环境

| 环境 | 行为 |
| --- | --- |
| 交互终端 | 内联 TUI |
| stdin 是管道或用 `-p "..."` | 无头模式：只输出最终回答到 stdout，进度写 stderr，权限按 `--permission` 参数，默认 plan |
| `--output json` | 输出 JSONL 事件流，供脚本解析 |
| `--rpc` | stdio JSONL 双向协议，供编辑器和其他 agent 嵌入 |
| `TERM=dumb` 或不支持 VT | 退化为行式输出，无颜色无活动区 |
| CI 环境变量存在 | 同无头模式 |

### 5.3 配置

只有两个位置，格式与 Claude Code 的 `settings.json` 相同：

- 用户级 `~/.lay/settings.json`（Windows 为 `%USERPROFILE%\.lay\`）
- 项目级 `<workspace>/.lay/settings.json`，另外只读识别 `.claude/settings.json`

内容：`model`、`providers`、`permissions.allow/deny`、`hooks`、`mcpServers`、`env`。密钥仍存 Windows 凭据管理器，用 `lay login <provider>` 录入。

### 5.4 命令行

```
lay                         交互模式，工作区为当前目录
lay "修复空配置"             带首条消息进入交互
lay -p "..."                无头模式
lay -c                      接上一次会话
lay -r                      选择会话恢复
lay --model X --permission yolo
lay login / lay models / lay mcp list
```

V1 的 29 个参数收缩到约 10 个。`--write`、`--allow-run`、`--session`、`--quote-task` 等全部移除。

---

## 6. 开发流程规范

这部分比代码更重要。

### 6.1 完成定义

一个改动在满足以下三条时即为完成，可以提交：

1. 能编译，无新警告。
2. `ctest` 快速集通过，总时长 < 60 秒，全部离线。
3. 你本人用它做了一次真实操作，看起来对。

不再需要：验收 JSON、人工手册、RC 编号、净机 VM、WSL、讲述人、多窗口尺寸矩阵、ConPTY 自动化录屏、线上 36 题门禁。

### 6.2 测试分层

| 层 | 内容 | 何时跑 |
| --- | --- | --- |
| 单元 | 解析器、SSE 解码、unicode、权限规则匹配、jsonl 读写 | 每次构建 |
| 回放 | 录制的真实 provider 流，离线回放到 loop，检查工具派发和最终消息 | 每次构建 |
| 冒烟 | 用一个真实模型跑 5 个固定小任务 | 发版前，手动 |

测试代码统一用 C++ + doctest，去掉 Python 依赖。

### 6.3 发版

- 版本号 `0.x.y`，不再用 rc 编号。
- GitHub Actions 的 windows-latest 本身就是一台干净机器，在上面构建、跑测试、打 zip，即完成"净机验证"。
- `CHANGELOG.md` 每版 3–5 行。

### 6.4 给 agent 的工作说明

在仓库根放一个不超过 60 行的 `CLAUDE.md`，写清楚：构建命令、测试命令、目录结构、代码风格、完成定义（6.1）。并明确写入：

- 不要创建验收 JSON 和手册。
- 不要为一个改动启动虚拟机或手工验证矩阵。
- 不要在 `build/` 下为每个阶段新建目录，统一用 `build/dev` 和 `build/release`。
- 设计文档只有本蓝图一份，有冲突时可以提出修改，不是"不可讨论"。

### 6.5 文档

只保留四份：`README.md`（安装和 30 秒上手）、`CLAUDE.md`（开发者和 agent）、`docs/BLUEPRINT_V2.md`（本文）、`CHANGELOG.md`。其余移入 `docs/archive/` 或删除。

---

## 7. 代码去留

V2 在新目录结构下重建，按模块从 V1 搬代码，不在旧文件上改。

| V1 文件 | 处理 | V2 位置 |
| --- | --- | --- |
| `src/model/provider_http.cpp` | 回收，去掉预算记账 | `src/provider/http.cpp` |
| `src/model/messages_codec.cpp` | 回收协议编解码，重写系统提示和上下文投影 | `src/provider/{anthropic,openai}.cpp` |
| `src/model/provider_config.cpp`、`provider_credentials.cpp` | 回收，配置改成 settings.json | `src/config/` |
| `src/platform/process_*.cpp` | 回收 | `src/platform/process.cpp` |
| `app/tui_unicode*`、`tui_text.hpp` | 回收 | `src/ui/unicode.*` |
| `app/tui_terminal.cpp` | 回收 VT 输入解析部分 | `src/ui/term.cpp` |
| `src/core/session.cpp` | 重写（loop 约 300 行） | `src/core/loop.cpp` |
| `src/tools/*` | 重写成 6 个工具 | `src/tools/` |
| `src/store/session_store.cpp` | 删除，改 JSONL | `src/core/session.cpp` |
| `app/tui.cpp`、`tui_workspace.cpp` | 重写成内联渲染 | `src/ui/` |
| `app/tui_accessible*`、`tests/e2e/tui_accessible*` | 删除 | — |
| `src/tools/citations.cpp`、`ctest.cpp` | 删除 | — |
| `bench/*.json`、`bench/*_MANUAL*.md`、`bench/p*_*.py` | 删除或归档 | — |
| `app/main.cpp`（1,372 行参数解析） | 重写，约 200 行 | `src/main.cpp` |

目标目录：

```
src/
  main.cpp
  core/      loop  context  permission  session  compact
  provider/  http  anthropic  openai
  tools/     read write edit bash grep glob
  ext/       skills commands hooks mcp agents plugins
  ui/        term render input unicode
  config/
tests/       doctest 单元 + 回放夹具
```

### 7.1 先处理未提交的 P24–P28 改动

工作区有 32 个修改和 17 个新文件尚未提交。V2 会重写这些模块，但它们是你最近的工作，应先保住：

```
git switch -c archive/v1-p28
git add -A && git commit -m "wip: P24-P28 snapshot before V2"
git switch main
git tag v1-final archive/v1-p28
```

之后 V2 在 `main` 上开新目录推进。V1 代码可随时从 tag 取回。

### 7.2 磁盘清理

以下都在 gitignore 范围内，不影响仓库：

| 路径 | 大小 | 建议 |
| --- | --- | --- |
| `build/p4-clean-windows/` | 16 GB | 删除。若还想保留 Win11 ISO 做别的用途，移到项目外 |
| `out/` 下历次 RC | 约 500 MB | 只留最新一个 zip，其余删除 |
| `build/` 下其余 200 多个阶段目录 | 约 400 MB | 全删，只保留 `build/dev`、`build/release` |
| `build/tooling`、`build/linux-*` | 约 250 MB | Python 虚拟环境和 Linux 依赖，V2 不需要可删 |

删除前请你确认，尤其是 VM 镜像，它们是不可恢复的。

---

## 8. 路线图

每个里程碑结束时都是可日常使用的版本。时间按一个人 + agent 估算。

| 里程碑 | 内容 | 完成标志 | 估时 |
| --- | --- | --- | --- |
| **M0 清场** | 7.1 快照、7.2 清理、写 `CLAUDE.md`、建新目录骨架、CI 跑通空测试 | `ctest` < 60s，项目目录 < 100 MB | 0.5 天 |
| **M1 能用** | loop、6 个工具、3 种 provider、JSONL 会话、内联 TUI、3 档权限、`/undo`、`-p` 无头 | 你用它改它自己的代码，不再打开 V1 | 4–5 天 |
| **M2 好用** | `CLAUDE.md`/`AGENTS.md` 加载、`@` 补全、`/` 面板、commands、skills、`/compact`、插话、Esc Esc 分叉 | 你现有的 `.claude/skills` 能直接调用 | 3 天 |
| **M3 生态** | MCP 客户端（stdio → HTTP）、hooks、settings.json 权限规则 | 接上一个常用 MCP server（如 filesystem 或 github）能正常调用 | 3–4 天 |
| **M4 扩展** | subagents（`task` 工具）、plugins 安装、`--rpc` 协议 | 装一个 Claude Code 插件能用 | 3 天 |
| **M5 打磨** | 性能预算全部达标、图片输入、费用统计、主题 | 3.8 的表格全绿 | 持续 |

M1 是关键。M1 之前不做任何扩展层工作，也不做 UI 细节打磨。

---

## 9. 需要你拍板的事

下列问题我给了默认选择，你不同意的话改这里即可：

1. **重建还是在 V1 上改**：默认重建内核，回收底层模块。理由见 1.3–1.4。
2. **SQLite 换 JSONL**：默认换。如果你确实需要跨会话检索，可以以后再加一个只读索引。
3. **全屏换内联**：默认换。这是砍掉读屏窗口和尺寸矩阵的前提。
4. **bash 用 Git Bash 还是 PowerShell**：默认优先 Git Bash，因为模型生成的命令大多是 POSIX 风格。没装 Git for Windows 时用 PowerShell。
5. **Linux**：保持能编译，不作为发布门槛。macOS 暂不考虑。
6. **项目名和命令名**：默认命令为 `lay`，配置目录 `~/.lay/`。

---

## 参考

- pi coding agent：默认只给模型 read、write、edit、bash 四个工具，其余能力靠 skills、扩展和包添加。[README](https://github.com/forkgitss/badlogic-pi-mono/blob/main/packages/coding-agent/README.md) · [A deep dive into Pi](https://flaviocopes.com/pi/)
- Agent Skills：Anthropic 于 2025 年 12 月作为开放标准发布 `SKILL.md` 格式，Claude Code、Codex、Copilot、Cursor、Gemini CLI 等已采用。启动时只读 name 和 description，匹配任务后再加载正文。[SKILL.md explained](https://blog.ascendingdc.com/blog/skill-md-explained/) · [AI Agent Skills Guide 2026](https://dev.to/serenitiesai/ai-agent-skills-guide-2026-build-skills-for-16-ai-tools-1jea)
- 本仓库数据：`bench/windows-p0.json`（启动和内存）、`du -sh build/* out/*`（磁盘）、`git log`（提交节奏）。
