# RC12 Windows Terminal 现场验收

使用 `out\lay-code-0.1.0-rc12-windows-x64\lay-code.exe`。候选 ZIP 的 SHA-256 为
`843383a205ef140490f94daf1fe0e9651d6668bd5dcd3a16c34edbec8166f9fe`。夹具运行在本机，只写自己的目录。

## 启动现场测试

1. 在 Windows Terminal 的 PowerShell 窗口 A 执行。窗口 A 留给夹具。

   ```powershell
   Set-Location 'D:\02_开发项目\lay-code'
   py -3.12 -B .\bench\p10_manual.py serve .\out\p19-manual-rc12
   ```

2. A 显示 `READY` 后，在新的 Windows Terminal PowerShell 窗口 B 执行。该窗口用于实际操作。

   ```powershell
   Set-Location 'D:\02_开发项目\lay-code'
   $fixture = (Resolve-Path .\out\p19-manual-rc12).Path
   $app = (Resolve-Path .\out\lay-code-0.1.0-rc12-windows-x64\lay-code.exe).Path
   $env:LOCALAPPDATA = Join-Path $fixture 'state'
   $launch = @('--tui','--workspace',(Join-Path $fixture 'workspace'),'--session','p10-rc8',
               '--provider-config',(Join-Path $fixture 'provider.json'),'--write','config.txt','--allow-run')
   & $app @launch
   ```

3. 另开一个 PowerShell 窗口 C，从项目目录随时查询夹具状态。

   ```powershell
   py -3.12 -B .\bench\p10_manual.py status .\out\p19-manual-rc12
   ```

## 拼音、剪贴板、窄窗和恢复

1. B 切到微软拼音，输入 `kongpeizhi`。确认候选窗跟着草稿输入框。在候选窗按空格选中「空配置」，再按 Enter。C 应显示 0 次请求。B 按 F2 后应显示第一条回复，C 应显示 1 次请求。
2. B 输入「修改空配置」并按 F2。出现 `p10-edit` 时将 B 缩窄。用 PgDn 或 End 翻到 `LONG-DIFF-END`。缩放前后都应能找到任务号和 `p10-edit`。
3. 在 C 执行 `Get-Content -Raw .\out\p19-manual-rc12\paste-sample.txt | Set-Clipboard`。B 按 Tab 进入审批输入，再用 Windows Terminal 原生粘贴快捷键粘贴。若 Windows Terminal 弹出多行粘贴确认，用鼠标确认该提示。C 应显示编辑派发 0 次，文件仍为 `original`。
4. B 切到英文键盘。用全新的 Tab、`y`、Enter 批准编辑。接着检查 `p10-run` 的程序、目录、版本和带空格的 `argument with spaces` 参数。将 B 恢复宽窗，调用 ID 应仍为 `p10-run`。
5. B 用 Tab、`y`、Enter 批准运行。等 C 的状态中 `process_starts = 1`，回 B 立即按 Ctrl+C。看到「已停止」后按 Esc 回 PowerShell，执行 `& $app @launch --resume`。应保留停止状态。
6. B 输入「确认之前的取消结果」并按 F2。应回复 `Previous cancellation found. No new tool call.`。C 的总结果应为 5 次请求、3 条任务、1 次编辑和 1 次命令派发，命令报告 `command cancelled`。

## 真实读屏器

用新的夹具目录 `p19-reader-manual-rc12` 复用启动步骤和前两条任务。保持在 `p10-edit` 的审批画面。

1. 按 Win+Ctrl+Enter 启动 Windows 讲述人，焦点回到 B。用讲述人键加 W 读窗口，记录能否听到当前待审批状态。
2. 按 F7 定位待审批动作，再读窗口。缩窄时用 PgDn 翻到差异，记录能否区分任务号、调用 ID、目标文件和修改范围。
3. 将焦点切回应用决定区，按 Tab 再空回车拒绝。记录讲述人是否读出结果。C 应显示未经批准的编辑派发为 0。退出讲述人用 Win+Ctrl+Enter。
4. 记录 Windows Terminal 版本、讲述人版本、各步骤可读内容及任何失焦。如果有读不到的字段，按 Esc 退出当前 TUI 后，用行式入口查同一会话。

   ```powershell
   $lineArgs = @($launch | Where-Object { $_ -ne '--tui' })
   & $app @lineArgs --resume --inspect
   ```

[讲述人键与读窗快捷键](https://support.microsoft.com/en-us/accessibility/windows/narrator/appendix-b-narrator-keyboard-commands-and-touch-gestures)
