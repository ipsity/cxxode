# Lay Code 0.1.0-rc5

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
