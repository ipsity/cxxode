# Lay Code 0.1.0-rc1

这是供本地验证的 Windows x64 便携候选包。解压后运行同目录下的 `lay-code.exe`。`manifest.json` 记录随包二进制的大小和 SHA-256，`licenses` 包含第三方许可文本。

解压目录里运行 `./lay-code.exe --help` 可查看入口。连接 Anthropic 时在本地环境设置 `ANTHROPIC_API_KEY`，再运行 `./lay-code.exe --workspace PATH --model MODEL`。选定 `--session ID` 可保留会话，`--write FILE` 和 `--allow-run` 分别启用需要逐次审批的编辑与命令。

## 验证范围

- 工作区读取与检索，逐次批准的单文件编辑与验证命令，以及 SQLite 会话恢复。
- 在相同工作区和授权范围内，以 `--resume` 重开会话。旧格式通过 `lay-code.exe --migrate-session ID` 离线迁移，程序会验证并保留升级前的 SQLite 备份。
- Windows 和 Ubuntu（WSL）均通过 6 项契约测试。便携包解压后在当前 Windows 主机上使用系统 PATH 启动，并从 NTFS 工作区读取了实际文件。全新 Windows 安装环境尚未复验。
- 真实 Anthropic 服务验收仍缺少 API 凭据。12 道质量题的离线夹具已验证。36 次真实模型试跑尚未执行，验收目标为至少 30/36。

Windows x64 NTFS 是便携包的验证平台。Linux 用于本地契约验证。运行项目自己的构建或测试时，项目所需编译器、Python、CMake 等由项目环境提供。

## Windows 参考机实测

使用 Ryzen 9 8945HX、32 GiB 内存的 Windows 11 主机，MinGW UCRT GCC 15.2.0 Release 构建。程序为 1,427,823 字节，随附 DLL 共 5,272,324 字节。原始样本位于源码仓库的 `bench/windows-p4.json`。

| 项目 | 结果 |
| --- | --- |
| 启动到提示符 | 100 次热启动 P95 为 **17.57 ms**。首次观测为 18.77 ms，系统缓存未重置 |
| 等待输入的私有内存 | 热启动样本最大 **1.34 MiB** |
| 双管道输出 | 约 100 MiB 输出在 **1.34 秒**内排空。每路保留 64 KiB，夹具宿主采样峰值为 **1.81 MiB**，子进程内存尚未采样 |
| 取消后的进程清理 | 五次驱动夹具清理用时 **17–23 ms**，计时从注入取消开始。交互终端的取消提示延迟尚未测量 |
| 回复显示 | 20 次回环夹具在首次文字后插入 120 ms 的后续事件间隔，首段发出到收到夹具输出的 P95 为 **137.07 ms** |

候选版在回复完整后一次显示。回环结果高于规划中的 30 ms 增量显示目标，本次验证范围为整轮显示。
