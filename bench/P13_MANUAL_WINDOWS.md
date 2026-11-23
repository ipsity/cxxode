# RC9 真人终端复测

在 **Windows Terminal** 中用 RC9 复测。测试目录固定为 `out\p13-manual-rc9`。夹具只访问本机，候选窗位置、焦点和原生剪贴板由现场观察。

## 启动

1. 在宽窗口 **C** 的 PowerShell 里执行下列命令。预期当前状态为 `requests = 0`。如果窗口还没打开，运行 `wt.exe -w new new-tab powershell.exe -NoLogo`。

   ```powershell
   $repo = 'D:\02_开发项目\lay-code'
   $fixture = Join-Path $repo 'out\p13-manual-rc9'
   $helper = Join-Path $repo 'bench\p10_manual.py'
   py -3.12 -B $helper status $fixture
   wt.exe -w new --size '48,16' new-tab --title 'P13 RC9' powershell.exe -NoLogo
   ```

2. 在刚打开的窄窗口 **B** 执行下列命令。尺寸应为 `48x16`。用下面的 `$app` 和 `$launch` 贯穿整轮测试。

   ```powershell
   $repo = 'D:\02_开发项目\lay-code'
   $fixture = Join-Path $repo 'out\p13-manual-rc9'
   $app = Join-Path $repo 'out\lay-code-0.1.0-rc9-windows-x64\lay-code.exe'
   $env:LOCALAPPDATA = Join-Path $fixture 'state'
   $launch = @('--tui', '--workspace', (Join-Path $fixture 'workspace'), '--session', 'p10-rc8', '--provider-config', (Join-Path $fixture 'provider.json'), '--write', 'config.txt', '--allow-run')
   '{0}x{1}' -f [Console]::WindowWidth, [Console]::WindowHeight
   & $app @launch
   ```

   RC9 ZIP SHA-256 为 `ed3494fb963df007330e46f613328c3a6c322ffe65470c7559d1922ffe6fc7a4`。`p10-rc8` 是夹具内固定的会话 ID，所用 EXE 由上面的 `$app` 决定。

## 操作与记录

1. 按 [真人手册第 2、3 节](P10_MANUAL_WINDOWS.md#2-用微软拼音提交一条中文任务) 测试微软拼音、窄窗长差异、原生粘贴和排队键。选词用空格或数字键。Enter 后应有 0 个请求，F2 后应有 1 个。原生粘贴前后，未经审批的编辑派发应为 0。测试手册中的 C 状态查询沿用当前目录。
2. 命令审批出现后，在 `48x16` 下翻阅带空格的参数。拖宽 B 并确认调用仍为 `p10-run`。在 C 先执行下列命令，让它在命令真正启动时提示。

   ```powershell
   $started = Join-Path $fixture 'workspace\run-started.txt'
   while (!(Test-Path -LiteralPath $started)) { Start-Sleep -Milliseconds 200 }
   Write-Host '命令已启动，回 B 按 Ctrl+C' -ForegroundColor Green
   [Console]::Beep(900, 300)
   ```

3. 在 B 通过 **Tab → 小写 y → Enter** 批准运行。等 C 的提示，立刻在 B 按 **Ctrl+C**，应看到 `已停止`。
4. 在 B 按 **Esc** 回到 PowerShell，运行 `& $app @launch --resume`。先确认重开仍显示 `已停止`，再追问「刚才运行是完成还是取消」，按一次 **F2**。预期回答 `Previous cancellation found. No new tool call.`。
5. 在 C 执行下面的命令。最终应有 **5 个请求、3 条任务、1 次运行**，并出现 `command cancelled` 及 `process_finished = false`。同时告知候选窗和焦点、窄窗与缩放、原生粘贴这三项的现场结果。

   ```powershell
   py -3.12 -B $helper status $fixture
   ```
