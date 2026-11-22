# Lay Code

Lay Code 是一个用 C++20 实现的本地代码助手。它支持工作区读取与检索、经审批的单文件编辑和本地验证命令。SQLite 记录持久会话的进度和结果。

## 离线演示

需要 C++20 编译器、CMake 3.24+ 和 Ninja。Windows 使用 MSVC 时，在 x64 开发者终端运行。

```powershell
cmake --preset release
cmake --build --preset release
ctest --preset release

'Can the parser accept an empty configuration?' | ./build/release/lay-code.exe
'Can the parser accept an empty configuration?' | ./build/release/lay-code.exe --truncated
```

Linux 使用 `./build/release/lay-code`。也可以直接启动程序，在提示符后输入一行 UTF-8 问题。正常轨迹退出码为 `0`。`--truncated` 模拟宿主收到一个完整读取提案后丢失回复终止事件，退出码为 `1`，派发次数为 `0`。

| 契约轨迹 | 停止原因 | 模型请求 | 读取派发 |
| --- | --- | ---: | ---: |
| 正常结束 | `end_turn` | 2 | 1 |
| 半截调用 | `protocol_error` | 1 | 0 |
| 回复提交前取消 | `cancelled` | 1 | 0 |
| 首次读取后耗尽请求预算 | `request_limit` | 1 | 1 |

契约测试对每条固定轨迹重放 100 次，逐次比较终态、已提交回复、调用和结果。CLI 端到端测试核对退出码和终端输出。

## 构建工作区模式

额外依赖 libcurl 7.85+、nlohmann-json 3.11+ 和 SQLite 3.37+。Windows 可在 MSVC x64 开发者终端使用 vcpkg manifest。Ubuntu 24.04 可安装 `libcurl4-openssl-dev`、`nlohmann-json3-dev`、`libsqlite3-dev` 和 `libssl-dev`，再直接配置同名 preset。契约测试需要 Python 3。

```powershell
cmake --preset readonly-release "-DCMAKE_TOOLCHAIN_FILE=$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build --preset readonly-release
ctest --preset readonly-release

./build/readonly-release/lay-code.exe --workspace . --provider openai --model YOUR_MODEL_ID --exclude private
```

将 `YOUR_MODEL_ID` 替换为账户可调用的模型 ID。Linux 可执行文件为 `./build/readonly-release/lay-code`。内置 `openai` 使用 Chat Completions 和 `OPENAI_API_KEY`，内置 `anthropic` 使用 Messages API 和 `ANTHROPIC_API_KEY`。没有设置默认服务商时，程序沿用 `anthropic`。

`--workspace` 选定文件范围，`--exclude` 可重复指定要排除的工作区相对路径。问题和工具选中的摘录会发给选定的服务商。`read` 列举一层目录或读取带行号的 UTF-8 文本，`search` 返回字面匹配的相对路径和行号。链接、重解析点、多硬链接文件、版本库元数据及常见凭据路径会被排除。工具各自报告截断或覆盖范围，`Ctrl+C` 取消当前请求。Windows 工作区使用本地 NTFS，Linux 需要 `openat2` 和 `/proc/self/fd`。

本地回环契约测试覆盖逐字节 SSE、检索和读取回填、批次预检、429 重试、断流及取消。

### 自定义服务商

Windows 在 `%LOCALAPPDATA%/LayCode/providers.json` 读取配置。Linux 使用 `${XDG_CONFIG_HOME:-$HOME/.config}/lay-code/providers.json`。`--provider-config PATH` 可以选用其他配置文件。

```json
{
  "default_provider": "passion",
  "providers": {
    "passion": {
      "protocol": "responses",
      "base_url": "https://api.passionapi.com",
      "api_key_env": "PASSION_API_KEY"
    }
  }
}
```

自定义服务商支持 `responses` 和 `chat-completions`。对于只填写域名的 HTTPS 地址，程序会补上 `/v1`。自定义 Chat Completions 默认使用 `max_tokens`，提供方支持 `max_completion_tokens` 和流末尾用量统计时，可以设置 `"openai_chat_options": true`。本地无鉴权服务可将 `api_key_env` 设为 `null`。

Windows 用户可在终端运行 `--set-provider-key`，隐藏录入的密钥会保存在当前用户的 Windows 凭据管理器，也可使用配置指定的环境变量。Linux 从环境变量读取密钥。

```powershell
./lay-code.exe --set-provider-key --provider passion
./lay-code.exe --list-models --provider passion
./lay-code.exe --workspace . --provider passion --model YOUR_MODEL_ID
```

模型列表来自服务商的 `/models` 接口。也可在便携包目录运行 `./setup_provider.ps1 -Provider passion`，录入密钥后从列表选模型，由脚本保存到该服务商的 `model` 字段。源码仓库里的同一脚本位于 `packaging/setup_provider.ps1`，可通过 `-Executable` 指定构建产物。设置默认服务商和对应模型后，`./lay-code.exe --workspace .` 会直接使用该配置。

真实回复的首段文字会通过 stderr 显示为 `[preview]`。完整且通过协议校验的回复及工具记录会由 stdout 输出。

## P2 单文件编辑与恢复

`--session ID --write FILE` 启用一个持久会话，并将写入范围限定为一条工作区相对文件路径。每次编辑先展示差异、已读取的版本和预期结果的摘要。输入 `y` 批准当前差异。非交互输入会在等待批准时暂停，可在终端重新打开同一会话。

以下离线演示在临时目录将 `empty_config = reject` 改为 `empty_config = allow`。启动后输入 `fix fixture`，核对差异再批准。

```powershell
$id = 'demo-' + [guid]::NewGuid().ToString('N')
$demo = New-Item -ItemType Directory -Path (Join-Path ([IO.Path]::GetTempPath()) $id)
'empty_config = reject' | Set-Content -Encoding utf8 -LiteralPath (Join-Path $demo.FullName 'config.txt')
./build/readonly-release/lay-code.exe --workspace $demo.FullName --model fixture-edit --session $id --write config.txt
./build/readonly-release/lay-code.exe --workspace $demo.FullName --model fixture-edit --session $id --write config.txt --resume
```

使用真实模型时，将 `fixture-edit` 换成账户可调用的模型 ID。重开时须提供相同的工作区、服务商、模型和文件范围。SQLite 会话存放在 Windows 的 `%LOCALAPPDATA%/LayCode/sessions` 或 Linux 的 `${XDG_STATE_HOME:-$HOME/.local/state}/lay-code/sessions`，目录位于工作区之外。每个会话由一个 CLI 持有写入锁。

收到 `effect_unknown` 时，宿主会暂停并显示恢复时观察到的文件状态及保留文件。核对后可用 `--resume --accept-unknown` 继续，原派发记录和未知结果会保留。已派发的编辑不会自动重发。

编辑支持至多 2 MiB 的普通 UTF-8 文件，保留原有 BOM 和统一的 LF 或 CRLF 换行。宿主在批准后和提交前复核文件版本。最后一次复核与替换之间仍存在外部编辑器写入的竞态。Windows 使用本地 NTFS，Linux 在同目录暂存文件并同步父目录。

## P3 构建与测试

`--session ID --allow-run` 启用验证命令，可与 `--write FILE` 一起使用。每次启动前，CLI 展示解析后的可执行文件、逐项参数、工作目录和超时。命令以当前账户权限直接启动，使用受限继承环境，标准输入关闭。使用真实模型时，命令输出的摘录会发给选定的提供方。

宿主单独展示退出状态、识别出的 CTest 测试结果和对应的已知编辑序号。退出码为 0 时，只有识别到测试器的通过记录才显示通过。再次编辑会将旧验证标为较早版本，外部修改由用户核对。每路输出保留头尾共 64 KiB，并记录实际遗漏的字节数。

Windows 使用 Job 回收子孙进程，Linux 使用进程组。超时或 `Ctrl+C` 会触发清理。进程清理无法确认，或命令已派发但结果未提交时，重开会话会报告未知并暂停。已派发的命令不会自动重发。

以下离线演示在临时目录复现空配置测试失败，应用单行编辑，再运行同一条测试。启动后输入 `fix and verify`，按提示逐次核对并批准。

```powershell
$id = 'verify-' + [guid]::NewGuid().ToString('N')
$demo = New-Item -ItemType Directory -Path (Join-Path ([IO.Path]::GetTempPath()) $id)
Copy-Item ./tests/fixtures/empty-config/* -Destination $demo.FullName
cmake -S $demo.FullName -B $demo.FullName -G Ninja
./build/readonly-release/lay-code.exe --workspace $demo.FullName --model fixture-verify --session $id --write config.txt --allow-run
```

## P0 基准与质量题集

[`bench/windows-p0.json`](bench/windows-p0.json) 保存 Windows 11 x64 参考机的 100 次热启动样本。MSYS2 UCRT64 GCC 15.2.0 Release 构建从创建进程到收到提示符的 P95 为 **14.99 ms**，等输入时的最大私有内存为 **1.12 MiB**。首次观测为 **15.87 ms**，测量使用管道接收 stdout，系统缓存状态未控制。

质量题集包含 4 道代码定位、4 道单文件修复、2 道保留未提交改动、2 道验证环境异常。`prepare` 把题目复制为独立 Git 仓库，目录须为新路径，隐藏断言保留在题集目录。

```powershell
python bench/startup.py ./build/release/lay-code.exe --compiler "MSYS2 UCRT64 GCC 15.2.0 (Rev13)"
python bench/quality.py list
python bench/quality.py verify
python bench/quality.py prepare fix-empty-config ./build/fix-empty-config
```

## P4 便携候选包与验收

Windows x64 便携包解压即可使用。发布范围、实测数据和待验收项见 [发布说明](RELEASE_NOTES.md)。本地打包需要 MSYS2 UCRT64 的 GCC、SQLite、nlohmann-json，以及 PATH 上的 CMake、Ninja 和 Python 3.11+。以下命令将依赖和编译产物写入构建目录或临时目录。

```powershell
$mingw = 'C:/msys64/ucrt64'
$deps = Join-Path $env:TEMP 'lay-code-p4-deps'
./packaging/build_windows.ps1 -Mingw $mingw -JsonPrefix $mingw -Scratch $deps
python -B packaging/windows.py --exe ./build/windows-rc1/lay-code.exe --mingw $mingw --curl (Join-Path $deps 'curl-install') --curl-license (Join-Path $deps 'curl-8.23.0/COPYING') --curl-source (Join-Path $deps 'curl-8.23.0.tar.xz') --json-license (Join-Path $mingw 'share/licenses/json/LICENSE.MIT') --output ./out --build 'MinGW UCRT Release' --version 0.1.0-rc3
```

长会话保留原始问题和最近的完整工具组，请求超限时以本地事实清单替换更早的轮次。完整证据仍保存在 SQLite。P2 和 P3 会话可用 `lay-code.exe --migrate-session ID` 离线升至格式 2，备份保存在会话目录。

完成服务商的密钥和模型配置后，可以在交互终端运行 36 次冻结评测。每题使用独立 Git 仓库，编辑和命令仍由终端逐次批准。报告包含验收结果、变更范围、调用次数，以及提供方返回的 token 计数。

```powershell
python -B bench/live_quality.py --cli ./build/windows-rc1/lay-code.exe --provider passion --model YOUR_MODEL_ID --output ./out/quality-run
```
