# Lay Code 0.1.0-rc15

P23 Windows x64 验收包。TUI 的 F11 可引用历史用户原文，F12 可选较新的现行决定。行式入口支持 `--quote-task` 和 `--current-decision-task`。来源任务、消息 ID 和正文 SHA-256 随提问保存，历史压缩时仍携带选中的原文。

首次保存引用后该会话使用格式 5。原有格式 4 会话可直接打开。P22 的配对实验保留了串行只读调度。

## Lay Code 0.1.0-rc14

P21 Windows x64 验收包。持久会话可用三次 `--write FILE` 授权三个精确路径。每份差异独立审批，已保存文件保留。CLI 和 TUI 可查看每个文件的状态，包括未开始和效果未知。恢复会话使用原来授权的路径清单。

## Lay Code 0.1.0-rc13

P20 Windows x64 验收包。CLI 与 TUI 可以按名称显示这次 CTest 的通过、失败、跳过、禁用和未运行结果。每项带捕获的 stdout 行号，运行详情保留筛选参数、退出码、时间和编辑版本。记录不完整时使用原有摘要。

## Lay Code 0.1.0-rc12

P18/P19 Windows x64 验收候选。F7 定位待审批动作，F8 定位最近失败验证，F9 查看本任务文件，F10 返回原阅读位置。历史阅读期间的新审批会在状态栏提示。窄窗可翻页，极小窗口可用同包行式入口查看。

同包的 Windows 11 NTFS 净机、真人 Windows Terminal 和线上质量门禁均已通过。线上评测通过 33/36，四项违规计数均为零。详见 [RC12 验收记录](bench/p19-rc12-validation.json)。

Windows 讲述人的审批范围和拒绝结果语音尚未确认。退出 TUI 后，用原会话参数在行式入口运行 `--resume --inspect` 可查看拒绝结果，已在 RC12 实测。

## Lay Code 0.1.0-rc11

P16 Windows x64 便携验收包。运行中的任务可按 F6 进入补充模式，再按 F2 提交。补充保留原任务和预算。已经派发的动作先结算，尚未派发的旧提案失效，新的验证或编辑动作另行审批。

通过 Responses、Chat Completions 和 Anthropic Messages 本地回环回放，以及 Windows ConPTY 和 Ubuntu 24.04 WSL PTY 的终端交互验证。验收记录位于源码仓库的 `bench/p16-validation.json`。

## Lay Code 0.1.0-rc10

P17 Windows x64 便携验收包。持久会话在每次模型 POST 前保存本任务的预算预留，包括自动重试。恢复保留已用额度及未知回执。相邻两次可核实的相同文件错误会暂停，任务收据会列出调用、已保存文件、验证记录和剩余额度。

行式 CLI 增加 `--resume --inspect`。TUI 用 Ctrl+O 打开收据。通过 Windows ConPTY、Ubuntu 24.04 WSL PTY 和本地回环提供方验证，结果记录于源码仓库的 `bench/p17-validation.json`。

## Lay Code 0.1.0-rc9

Windows x64 便携候选包。一个持久会话可用两次 `--write FILE` 明确授权源码和测试文件。每份差异单独审批。CLI 和 TUI 重开时显示逐文件进度、原始差异、批准记录和历史验证版本。

适用环境为 Windows 11 x64 本地 NTFS。Windows Terminal 用 F2 提交任务、Ctrl+C 停止、Esc 退出。命令执行需启用 `--allow-run` 并逐次批准。效果未知时先核对文件，再决定是否继续。旧版单文件会话可用原授权路径直接重开。

发行评测使用 Passion Responses 的 `gpt-6.1-sol`、Windows 原生 ConPTY 和 PyWinPTY、Windows Terminal 与微软拼音。来源提交和二进制摘要在 `manifest.json`，ZIP 摘要位于旁附的 SHA-256 文件。发行状态和各项实测见源码仓库的 `bench/p13-rc9-validation.json`。

## Lay Code 0.1.0-rc8

Windows x64 便携候选包用于中文终端验收。TUI 重开会话时保留 Ctrl+C 的取消状态。CLI 和 TUI 同时显示历史引用、读取版本以及验证命令覆盖的编辑版本。

会话按请求记录服务商返回的用量字段和实际上传字节。长会话压缩上下文时，有限的历史摘要空间会优先保留各文件的首次读取版本，便于追问早期证据。

Windows 与 WSL 的契约测试覆盖多轮追问、48×16 审批翻阅、取消后重开和防止排队输入触发审批。Windows Terminal 的微软拼音候选窗和原生剪贴板仍待真人验收。用量与长会话的测量样本位于源码仓库的 `bench/p8-usage-audit.json` 和 `bench/p11-mixed-history.json`。

## Lay Code 0.1.0-rc7

Windows x64 便携候选包加入显式 `--tui` 工作区入口。行式 CLI 和 TUI 共用 SQLite 会话，编辑与命令逐次审批。按 F2 提交新任务，按 Ctrl+C 停止当前任务。效果未知时可先核对，再结清或用行式入口恢复。

从旧数据库升级时运行 `lay-code --migrate-session ID`。程序会保留升级前的备份。回退到旧版程序时，用原格式备份打开旧记录，保留含新任务的 v3 数据库。终端用法和启动参数见 [README](README.md)。

缩短了 TUI 活动期间的界面等待，改善首段预览和任务状态更新。

### P7 验收

- Windows 11 x64 NTFS 净机解包通过。行式 CLI 读取真实文件，TUI 完成审批编辑和后续任务，行式 CLI 能重开同一会话。
- Windows 和 WSL 各通过 10 项契约测试。双入口对照覆盖续跑测试、外部改写、拒绝记忆和未知效果结清。
- Passion `gpt-6.1-sol` 经 Responses 完成 36 次冻结评测，通过 34 次，四项违规计数均为零。线上双任务抽查的两种入口都完成了相同的调用顺序。
- 参考机上，行式启动 P95 为 19.05 ms，原生 ConPTY 的 TUI 启动 P95 为 82.72 ms。输入、预览和取消反馈的 P95 分别为 0.61、5.34、2.58 ms。输出洪峰约 100 MiB，两路各保留 64 KiB。经 PyWinPTY 封装测得的启动和原生 ConPTY 样本分别记录。

包摘要、耗时样本、用量和未通过题目见源码仓库的 [RC7 验收记录](bench/p7-rc7-validation.json)。键盘和 Unicode 输入在 ConPTY 和 PTY 上自动验证，Windows Terminal 的真实中文输入法候选窗口待人工复核。

## Lay Code 0.1.0-rc5

在编辑或命令审批时按 Ctrl+C 会暂停会话，待审批动作可在重开时继续审核。Windows 使用控制台事件响应取消，Linux 使用可中断的终端等待。在线会话记录 libcurl 返回的真实 HTTP 首字节耗时，旧会话仍可离线读取。

## P4 验收

便携包面向 Windows x64 本地 NTFS。解压即可运行 CLI。运行项目构建或测试时，编译器和测试器由目标项目环境提供。全新 Windows 复验、性能与真实模型质量记录分别位于源码仓库的 `bench/p4-rc5-clean-windows.json`、`bench/p4-rc5-performance.json` 和 `bench/p4-rc5-quality.json`，记录通过 SHA-256 关联候选二进制或 ZIP。

| 指标 | 实测 |
| --- | --- |
| 热启动和空闲内存 | Windows 100 次 P95 为 23.67 ms，最大私有内存 1.37 MiB。Ubuntu 24.04 WSL 100 次 P95 为 8.29 ms，最大私有驻留内存 3.22 MiB。首次观察值单列在样本内，系统缓存未重置 |
| 本地首段预览 | Responses 回环各 20 次。Windows 管道 P95 为 0.10 ms，WSL 管道为 0.52 ms |
| 双管道输出 | Windows CLI 在 518 ms 内排空 100 MiB，每路保留 64 KiB。父进程私有内存比待审批基线增加 0.93 MiB，子进程采样峰值 8.15 MiB。测量期间暂停读取终端 |
| Ctrl+C | Windows ConPTY 在模型等待、审批和输出洪峰各测 20 次，提示 P95 分别为 64.87、26.79、15.39 ms。输出洪峰的最长退出时间为 654.93 ms，并确认父子进程均已退出 |

Windows 无窗口子进程由 Job 回收，取消时直接终止进程树。Linux 向进程组发送 SIGTERM，超出 2 秒宽限时升级为 SIGKILL。清理状态无法确认时，会话报告未知并暂停。

## Lay Code 0.1.0-rc4

CLI 为相邻的文件和行号字段补充 `[citation] path:line`，前提是成功的文件读取返回了对应行。评测分别记录终端显示与模型原文的引用格式。

## Lay Code 0.1.0-rc3

- CLI 收到模型的首段文字后通过 stderr 显示 `[preview]`。完整答复及工具派发仍须通过协议校验。
- Passion API 有时会在 Responses 流结束时替换响应 ID。只有完整闭合的输出项与终止事件逐项匹配，宿主才提交该轮回复。
- Windows 回环的 20 次 Responses 样本中，从首段 SSE 发出到读取预览的 P95 为 **0.18 ms**。夹具间隔为 120 ms，目标为 30 ms，采样使用 stderr 管道。原始样本位于源码仓库的 `bench/windows-rc3-preview.json`。

## Lay Code 0.1.0-rc2

Windows x64 便携候选包新增 OpenAI 和自定义服务商。自定义配置支持 Responses 和 Chat Completions，可查询模型列表并保存默认模型。Windows 密钥可通过隐藏输入保存到当前用户的凭据管理器。

配置位置及调用示例见 [README](README.md)。契约测试使用本地回环服务覆盖两种 OpenAI 协议的工具调用和流中断。真实服务的调用取决于账户凭据及模型权限。下面保留 RC1 的参考机数据。

## Lay Code 0.1.0-rc1

这是供本地验证的 Windows x64 便携候选包。解压后运行同目录下的 `lay-code.exe`。`manifest.json` 记录随包二进制的大小和 SHA-256，`licenses` 包含第三方许可文本。

解压目录里运行 `./lay-code.exe --help` 可查看入口。连接 Anthropic 时在本地环境设置 `ANTHROPIC_API_KEY`，再运行 `./lay-code.exe --workspace PATH --model MODEL`。选定 `--session ID` 可保留会话，`--write FILE` 和 `--allow-run` 分别启用需要逐次审批的编辑与命令。

### RC1 验证范围

- 工作区读取与检索，逐次批准的单文件编辑与验证命令，以及 SQLite 会话恢复。
- 在相同工作区和授权范围内，以 `--resume` 重开会话。旧格式通过 `lay-code.exe --migrate-session ID` 离线迁移，程序会验证并保留升级前的 SQLite 备份。
- Windows 和 Ubuntu（WSL）均通过 6 项契约测试。便携包解压后在当前 Windows 主机上使用系统 PATH 启动，并从 NTFS 工作区读取了实际文件。全新 Windows 安装环境尚未复验。
- 真实 Anthropic 服务验收仍缺少 API 凭据。12 道质量题的离线夹具已验证。36 次真实模型试跑尚未执行，验收目标为至少 30/36。

Windows x64 NTFS 是便携包的验证平台。Linux 用于本地契约验证。运行项目自己的构建或测试时，项目所需编译器、Python、CMake 等由项目环境提供。

### RC1 Windows 参考机实测

使用 Ryzen 9 8945HX、32 GiB 内存的 Windows 11 主机，MinGW UCRT GCC 15.2.0 Release 构建。程序为 1,427,823 字节，随附 DLL 共 5,272,324 字节。原始样本位于源码仓库的 `bench/windows-p4.json`。

| 项目 | 结果 |
| --- | --- |
| 启动到提示符 | 100 次热启动 P95 为 **17.57 ms**。首次观测为 18.77 ms，系统缓存未重置 |
| 等待输入的私有内存 | 热启动样本最大 **1.34 MiB** |
| 双管道输出 | 约 100 MiB 输出在 **1.34 秒**内排空。每路保留 64 KiB，夹具宿主采样峰值为 **1.81 MiB**，子进程内存尚未采样 |
| 取消后的进程清理 | 五次驱动夹具清理用时 **17–23 ms**，计时从注入取消开始。交互终端的取消提示延迟尚未测量 |
| 回复显示 | 20 次回环夹具在首次文字后插入 120 ms 的后续事件间隔，首段发出到收到夹具输出的 P95 为 **137.07 ms** |

候选版在回复完整后一次显示。回环结果高于规划中的 30 ms 增量显示目标，本次验证范围为整轮显示。
