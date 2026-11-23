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

真实回复的首段文字会通过 stderr 显示为 `[preview]`。完整且通过协议校验的回复及工具记录会由 stdout 输出。回复分开给出相邻的文件和行号字段时，CLI 可以根据本轮读取记录补出 `[citation] path:line`。

## P2 单文件编辑与恢复

`--session ID` 启用持久会话。用一次或两次 `--write FILE` 可以授权最多两个精确的工作区相对文件路径。每次编辑先展示该文件的差异、已读取的版本和预期结果摘要。输入 `y` 批准当前差异。非交互输入会在等待批准时暂停，可在终端重新打开同一会话。

```powershell
lay-code.exe --tui --workspace . --session pager --write src/pager.py --write tests/test_paging.py --allow-run
```

双文件会话中，每份差异、批准决定和提交结果分别保存。第二份修改被拒绝或遇到版本冲突时，第一份已提交的修改保留。重开时传入相同的两条路径，顺序可以互换。CLI 和 TUI 都展示当前任务的逐文件进度。展开 TUI 中对应的编辑记录可回看原差异和批准决定。

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
python -B packaging/windows.py --exe ./build/windows-rc1/lay-code.exe --mingw $mingw --curl (Join-Path $deps 'curl-install') --curl-license (Join-Path $deps 'curl-8.23.0/COPYING') --curl-source (Join-Path $deps 'curl-8.23.0.tar.xz') --json-license (Join-Path $mingw 'share/licenses/json/LICENSE.MIT') --output ./out --build 'MinGW UCRT Release' --version 0.1.0-rc5
```

长会话保留每个任务的用户原文和最近的完整工具组。请求超限时以本地事实清单替换更早的轮次，完整证据仍保存在 SQLite。

完成服务商的密钥和模型配置后，可以在交互终端运行 36 次冻结评测。每题使用独立 Git 仓库，编辑和命令仍由终端逐次批准。报告包含验收结果、变更范围、调用次数，以及提供方返回的 token 计数。定位题接受 `path:line` 或相邻的文件和行号字段。报告分别用 `citation_format` 和 `model_citation_format` 记录 CLI 输出与模型原文的格式合规次数。

评测的 `first_byte_us` 记录每次 HTTP 尝试的首字节耗时，单位为微秒。使用 `bench/terminal_cancel.py` 可在 Windows ConPTY 复测真实 Ctrl+C 和双管道输出。该测量脚本需要 Python 和 pywinpty 3.1.2。

```powershell
python -B bench/live_quality.py --cli ./build/windows-rc1/lay-code.exe --provider passion --model YOUR_MODEL_ID --output ./out/quality-run
```

## 在持久会话中追加任务

首次提问仍使用 `--session` 和 `--question`。当前任务结清后，可以在另一个终端进程中追加任务。保留原来的工作区、模型和权限参数。存在待批准或待核对的动作时，先恢复原任务。

```powershell
lay-code.exe --workspace . --session parser --follow-up "刚才的判断依据是哪一行？"
lay-code.exe --workspace . --session parser --resume
```

追加时可加 `--no-write`，禁用本任务的编辑工具。若原会话启用了 `--allow-run`，运行命令仍需逐次批准。追加的原文先保存再请求模型。中断后用 `--resume` 恢复同一条任务，接续已记录的请求数、工具调用数和活动时间。等待审批的时间不计入 15 分钟额度。

多任务会话会随引用显示来源任务、轮次、文件版本和读取时间。旧任务的引用以及当前文件版本无法核对的引用标为 `[historical citation]`。追加任务后重新读取文件，即可按当前版本引用。`Ctrl+C` 中断后，已保存的工具结果会随原任务恢复。

网络失败、模型拒答、上下文超限或等待审批时，可以用 `--resume --close-task` 离线结束当前任务，保留已保存结果并将未执行提案标成跳过。核对 `effect_unknown` 的记录后，可用 `--resume --accept-unknown --close-task` 离线结清并保留未知状态，再追加任务。

旧会话需先用 `lay-code.exe --migrate-session ID` 从格式 1 或 2 离线升级到格式 3。原格式备份会保存在会话目录。需要回到旧版程序时，请保全格式 3 数据库，再使用升级前的备份。

## 离线终端演示

在交互终端执行以下任一命令。四个场景均使用固定记录，审批和提交都只在演示中生效。

```powershell
lay-code.exe --tui --demo normal
lay-code.exe --tui --demo approval
lay-code.exe --tui --demo failure --mono
lay-code.exe --tui --demo unknown --ascii
```

方向键和 PgUp/PgDn 翻阅记录，Ctrl+O 展开或收起详情。待审批时用 Tab 进入决定框，输入小写 `y` 或 `yes` 后回车批准，空回车拒绝。Ctrl+C 停止当前动作，Esc 退出，决定框内的 Esc 返回审阅。草稿用 F2 提交，按 Enter 保留草稿。需要纯单色时加 `--mono`，需要 ASCII 排版时加 `--ascii`。

## 用终端界面处理工作区任务

使用原会话的工作区、模型和权限参数。以下会话允许编辑 `config.txt` 和提议运行命令，每次编辑和运行仍单独审批。

```powershell
lay-code.exe --tui --workspace . --session parser --write config.txt --allow-run
lay-code.exe --tui --workspace . --session parser --write config.txt --allow-run --resume
```

用 F2 提交新任务。上一条任务结清后，可在同一窗口输入并提交后续任务。有写权限的会话可用 F4 禁用下一条任务的编辑工具。用 ↑↓ 选择记录，Ctrl+O 展开对应工具输出，PgUp/PgDn 翻页。忙时可以编辑下一条草稿。

`--resume` 先展示已保存的记录。按 F5 继续原任务，按 F2 提交新草稿。以前用 F2 恢复任务的用户，可按暂停屏幕上的 F5 提示继续。Ctrl+C 停止当前任务，已提交的工具结果会保留。若记录显示效果未知，核对工作区后连续两次按 F3 可离线结清。也可以用同版本的行式 CLI 恢复同一会话。

忙时可以编辑下一条任务草稿，按 F2 会提示草稿未发送。草稿只保存在当前窗口，退出会丢弃。手动录入上限为 4,096 字节，粘贴送入的换行和制表符会变为空格。终端太小时可先放大，再返回原审批。若在审批输入框中缩放窗口，需用 Tab 重新进入输入框，再输入批准决定。

任务暂停时连续两次按 F3 可离线结束本任务。若最终回复保存失败，界面会单独标出未保存的回复，以 SQLite 检查点决定能否开始下一条任务。

同一会话的两个入口都会标出历史引用及其读取版本。展开 TUI 的引用卡或验证记录，可查看完整证据和观察时间。验证记录保留当时的编辑版本和结果，当前工作区的整体测试状态单独显示为未知。

### 核对会话用量

源码目录内可用 Python 读取已有会话的 SQLite 文件。报告按任务列出请求序号、请求体和实际上传字节、可复算的 token 小计及缺报次数。

```powershell
python -B bench/usage_audit.py "$env:LOCALAPPDATA/LayCode/sessions/session-parser.sqlite3"
```

`provider_usage` 保存供应方上报的已识别数值字段，字段缺失和回报零各自保留。Responses 与 Chat 的缓存细项已包含在主计数中。Claude 输入小计使用已报告的输入、缓存读和缓存写分项。报告的 `verified_cost` 留空，费用以服务商的可核对账单为准。

开发构建可通过 `-DLAY_BENCHMARK_METRICS=ON` 启用本地投影与检查点计时。Windows 上用 PyWinPTY 运行 `python -B bench/mixed_history.py PATH_TO_EXE PATH_TO_CTEST`，可在固定回环服务下复现 100 任务的混合轨迹与重开采样。源码仓库中的 `bench/p11-mixed-history.json` 保留对照结果。
