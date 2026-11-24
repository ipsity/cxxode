# RC8 Windows Terminal 真人验收

全程使用同一个 RC8。夹具在本机 `127.0.0.1` 返回固定回复。配置文件、测试进程和会话数据都放在 `out\p10-manual-01`。先准备 Windows Terminal、微软拼音和 Python 3.12。

## 1. 准备三个窗口

1. 从开始菜单打开 **Windows Terminal**，新建 **Windows PowerShell** 标签页。这个窗口叫 **A（夹具）**。执行以下命令，记下 ZIP 摘要和本机版本。

   ```powershell
   $repo = 'D:\02_开发项目\lay-code'
   $zip = Join-Path $repo 'out\lay-code-0.1.0-rc8-windows-x64.zip'
   Get-FileHash -LiteralPath $zip -Algorithm SHA256
   (Get-AppxPackage Microsoft.WindowsTerminal).Version
   (Get-Item "$env:SystemRoot\System32\InputMethod\CHS\ChsIME.exe").VersionInfo.FileVersion
   py -3.12 --version
   ```

   ZIP 的预期 SHA-256 为 `573702860c2299c60bc26cb27d99d6041f75ff9e83db890bdee1e6c4f6070335`。参考机已记录的 Terminal 和微软拼音文件版本分别为 `1.25.3131.0` 和 `10.0.26100.4484`。以你现场实际看到的输出为准。

2. 在 A 执行下一段。它打开 **B（被测界面）** 和 **C（检查计数）**，再启动固定夹具。如果 `p10-manual-01` 已存在，把下文三个代码块中的目录名都改成 `p10-manual-02`。等待 A 显示 `READY`，让 A 一直运行到最后。

   ```powershell
   wt.exe -w new --size '48,16' new-tab --title 'P10 RC8' powershell.exe -NoLogo
   wt.exe -w new new-tab --title 'P10 检查' powershell.exe -NoLogo
   py -3.12 -B (Join-Path $repo 'bench\p10_manual.py') serve (Join-Path $repo 'out\p10-manual-01')
   ```

3. 到 C 执行下列命令。第一次应看到 `requests = 0`、`tasks = 0`、`config = original` 和 `process_starts = 0`。之后在 C 按 **↑、Enter** 就能重跑最后一条状态查询。A 会按顺序打印每个请求的任务号和返回动作。

   ```powershell
   $repo = 'D:\02_开发项目\lay-code'
   $fixture = Join-Path $repo 'out\p10-manual-01'
   $helper = Join-Path $repo 'bench\p10_manual.py'
   py -3.12 -B $helper status $fixture
   ```

4. 到 B 执行下列命令，核对启动前显示的 `48x16`，再等界面显示 `等待任务`。若尺寸有偏差，按 **Esc** 返回 PowerShell，还原最大化状态或拖动窗口边框，再执行打印尺寸和启动的最后两行。[Windows Terminal 的启动尺寸参数](https://learn.microsoft.com/zh-cn/windows/terminal/command-line-arguments)

   ```powershell
   $repo = 'D:\02_开发项目\lay-code'
   $fixture = Join-Path $repo 'out\p10-manual-01'
   $app = Join-Path $repo 'out\lay-code-0.1.0-rc8-windows-x64\lay-code.exe'
   $env:LOCALAPPDATA = Join-Path $fixture 'state'
   $launch = @('--tui', '--workspace', (Join-Path $fixture 'workspace'), '--session', 'p10-rc8', '--provider-config', (Join-Path $fixture 'provider.json'), '--write', 'config.txt', '--allow-run')
   '{0}x{1}' -f [Console]::WindowWidth, [Console]::WindowHeight
   & $app @launch
   ```

## 2. 用微软拼音提交一条中文任务

1. 在 B 用 **Win+空格** 切换到 **中文（简体，中国）· 微软拼音**。确认输入状态显示「中」。若是「英」，按一次 **Shift**。如果列表缺少微软拼音，在 Windows 设置的 **时间和语言 → 语言和区域 → 中文（简体，中国）→ 语言选项 → 键盘** 添加它。[微软拼音设置与候选键](https://support.microsoft.com/zh-cn/windows/hardware/input-devices/microsoft-simplified-chinese-ime)
2. 在 B 底部 `>` 后键入 `ceshizhongwen`，等候选窗出现。观察候选是否能读清、输入文字是否被遮挡、光标是否仍在草稿区。
3. 候选窗打开时按一次 **Enter**。微软拼音默认关闭候选窗并保留原始拼音。到 C 核对 `requests = 0`、`tasks = 0`。切回 B，用 **Backspace** 清空这次输入。
4. 再键入 `ceshizhongwen`，用 **空格** 或候选编号选择「测试中文」。确认 B 的 `>` 后显示汉字。再按一次 **Enter**，到 C 核对请求数和任务数仍为 **0**。
5. 切回 B，只按一次 **F2**。等到 `已完成` 和 `Chinese input test received.`。C 应显示 `requests = 1`、`tasks = 1`。如果键盘将 F2 映射成媒体键且界面没有提交，使用 **Fn+F2**。

## 3. 检查原生粘贴、排队键和长差异

1. 在 C 执行 `notepad.exe (Join-Path $fixture 'paste-sample.txt')`。记事本第一行应是 `y`，第二行是 `second line of paste`。按 **Ctrl+A、Ctrl+C**，复制原生多行文本。关闭记事本。回 C 再执行一次 `py -3.12 -B $helper status $fixture`，后续用 **↑、Enter** 查询，避免覆盖剪贴板。
2. 到 B 按 **Ctrl+Shift+V** 粘贴。若 Terminal 弹出多行确认，选择继续粘贴。确认 C 的 `requests` 仍是 **1**。回 B，在草稿末尾按住 **Backspace**，直到 `>` 后为空。[Windows Terminal 粘贴快捷键](https://learn.microsoft.com/zh-cn/windows/terminal/customize-settings/actions)
3. 在 B 切到「英」或英文键盘，输入 `edit safety fixture`，按一次 **F2**。在出现审批前连续按 **Tab、y、Enter**。夹具会将编辑提议延迟约 4 秒。等 `待批准 · 编辑 config.txt` 出现，去 C 查 `requests = 3`、`tasks = 2`、`config = original`、`edit_dispatches = 0`、`run_dispatches = 0`。如果审批弹出前没来得及按键，记录该项未测。
4. 回 B，在审阅画面还没按 Tab 时按 **Ctrl+Shift+V**。仍应停留在 `p10-edit` 审阅，C 的 `edit_dispatches` 应为 **0**。
5. 回 B 按 **Tab**，等批准输入框出现，再按 **Ctrl+Shift+V** 粘贴同样的文本。若 Terminal 弹出多行确认，选择继续粘贴。预期界面提示已忽略粘贴或排队回车，并退回审阅。C 仍应显示 `requests = 3`、`config = original`、`edit_dispatches = 0`。若编辑意外通过，立即按 **Ctrl+C**，记录该项失败和当时计数。
6. 回到仍为 `48×16` 的 B，按 **Home** 翻到顶部，看到 `任务 2 / p10-edit`。用 **PgDn** 逐页查看原来的 `empty_config = reject` 和新增的 `empty_config = allow`。用 **End** 查末尾的 `LONG-DIFF-END`，用 **↑、↓** 微调位置。记下是否能读完版本、路径和差异。
7. 手动按 **Tab**，等批准输入框出现。键入小写 **y**，看到 `> y` 后再按 **Enter**。下一条命令审阅出现时，C 应显示 `config = edited`、`edit_dispatches = 1`、`requests = 4`。

## 4. 核对带空格的参数、缩放并取消

1. 在仍为 `48×16` 的 B 看到 `待批准 · 运行` 后，按 **Home** 查看 `任务 2 / p10-run`，用 **PgDn** 或 **End** 查看参数。确认 `参数 1 "hold check.py"` 和 `参数 2 "argument with spaces"` 各显示为一个参数。逐页可看到执行文件、工作目录和超时。C 此时应显示 `run_dispatches = 0`、`process_starts = 0`。
2. 拖动 B 的边框，让窗口变宽。按 **Home**，调用 ID 仍应为 `p10-run`。再按 **End** 核对带空格的两个参数。
3. 按 **Tab**，等批准输入框出现。手动按小写 **y**，看到 `> y` 后按 **Enter**。立即到 C 用 **↑、Enter** 查 `run_dispatches = 1`、`process_starts = 1`、`process_finished = false`。夹具留有约 **60 秒**供你取消。
4. 回 B，确认没有选中屏幕文字，按一次 **Ctrl+C**。预计看到 `正在停止` 或直接看到 `已停止`。去 C 核对 `requests = 4`、`process_starts = 1`、`process_finished = false`、`run_results` 包含 `command cancelled`，第二条任务状态为 `cancelled`。

## 5. 重开会话并保存结果

1. B 按 **Esc** 回到 PowerShell。保持 A 运行。在 B 执行 `& $app @launch --resume`。等 TUI 显示 `已停止`。C 的 `requests` 仍应为 **4**。在 B 按 **End、Ctrl+O** 展开上次命令记录，翻阅到 `command cancelled at edit revision 1`。
2. 在 B 用微软拼音输入「刚才运行是完成还是取消」，选词后再按一次 **F2**。预期回答 `Previous cancellation found. No new tool call.`。正文如有折行，按 **End、Ctrl+O** 展开。
3. C 用 **↑、Enter** 看最终状态。`request_trace` 应依次为 `message → read → edit → run → message`。

   | 字段 | 预期 |
   | --- | --- |
   | `requests` / `tasks` | `5` / `3` |
   | `task_states` | `end_turn`、`cancelled`、`end_turn` |
   | `config` / `edit_dispatches` | `edited` / `1` |
   | `run_dispatches` / `process_starts` | `1` / `1` |
   | `process_finished` | `false` |
   | `run_results` | `command cancelled` |

4. 在 C 保存记录。另记下实际版本、候选窗是否遮挡、粘贴有没有触发输入框退出，以及哪一步有异常。

   ```powershell
   py -3.12 -B $helper status $fixture | Set-Content -Encoding UTF8 (Join-Path $fixture 'manual-status.json')
   ```

5. B 按 **Esc** 退出。A 按 **Ctrl+C** 停止本机夹具。结果和测试目录保留供核对。
