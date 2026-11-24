param(
    [string]$Archive = (Join-Path $PSScriptRoot 'candidate.zip'),
    [string]$SerialPort,
    [string]$TuiProbe,
    [string]$DualFixture
)

$ErrorActionPreference = 'Stop'
$port = $null
if ($SerialPort) {
    $port = New-Object IO.Ports.SerialPort $SerialPort,115200
    $port.Open()
}
try {
    $os = Get-CimInstance Win32_OperatingSystem
    $hash = Get-FileHash -LiteralPath $Archive -Algorithm SHA256
    $scratch = Join-Path $env:TEMP ('lay-p4-clean-' + [guid]::NewGuid().ToString('N'))
    $package = New-Item -ItemType Directory -Path (Join-Path $scratch 'package') -Force
    Expand-Archive -LiteralPath $Archive -DestinationPath $package.FullName
    $applications = @(Get-ChildItem -LiteralPath $package.FullName -Filter lay-code.exe -File -Recurse)
    if ($applications.Count -ne 1) { throw 'Expected one extracted executable.' }
    $exe = $applications[0].FullName
    $manifest = Get-Content -LiteralPath (Join-Path $applications[0].DirectoryName 'manifest.json') -Raw | ConvertFrom-Json
    $exeHash = (Get-FileHash -LiteralPath $exe -Algorithm SHA256).Hash
    if ($exeHash -ne $manifest.binaries.'lay-code.exe'.sha256) { throw 'Executable hash mismatch.' }

    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot;$env:SystemRoot\System32\WindowsPowerShell\v1.0"
    $tools = @(Get-Command cmake.exe,ninja.exe,gcc.exe,cl.exe,python.exe -ErrorAction SilentlyContinue)
    if ($tools.Count) { throw ("Unexpected tools in guest PATH: " + ($tools.Name -join ', ')) }
    $env:LOCALAPPDATA = Join-Path $scratch 'isolated-state'
    $workspaceName = 'workspace-' + [char]0x9a8c + [char]0x6536
    $workspace = New-Item -ItemType Directory -Path (Join-Path $scratch $workspaceName)
    $file = Join-Path $workspace.FullName 'config.txt'
    $original = 'empty_config = reject' + [Environment]::NewLine
    [IO.File]::WriteAllText($file, $original, [Text.UTF8Encoding]::new($false))
    $help = (& $exe --help | Out-String)
    if ($LASTEXITCODE -ne 0 -or $help -notmatch 'Usage: lay-code') { throw 'CLI did not start.' }

    $arguments = @('--workspace', ('"' + $workspace.FullName + '"'), '--model', 'fixture-edit',
                   '--session', 'clean-p4', '--write', 'config.txt', '--question', '"fix fixture"')
    $launch = New-Object Diagnostics.ProcessStartInfo
    $launch.FileName = $exe
    $launch.Arguments = $arguments -join ' '
    $launch.UseShellExecute = $false
    $launch.CreateNoWindow = $true
    $launch.RedirectStandardInput = $true
    $launch.RedirectStandardOutput = $true
    $launch.RedirectStandardError = $true
    $launch.StandardOutputEncoding = [Text.Encoding]::UTF8
    $process = [Diagnostics.Process]::Start($launch)
    try {
        $process.StandardInput.Close()
        $stdout = $process.StandardOutput.ReadToEndAsync()
        $stderr = $process.StandardError.ReadToEndAsync()
        if (!$process.WaitForExit(15000)) {
            $process.Kill()
            throw 'Packaged workspace read timed out.'
        }
        $code = $process.ExitCode
        $output = $stdout.GetAwaiter().GetResult()
        $diagnostic = $stderr.GetAwaiter().GetResult()
    } finally {
        $process.Dispose()
    }
    if ($code -ne 1 -or $output -notmatch '\[read\] config\.txt \(ok\)' -or $output -notmatch 'approval_required') {
        throw "Unexpected smoke result ($code) $output $diagnostic"
    }
    if ([IO.File]::ReadAllText($file) -ne $original) { throw 'Smoke test altered the fixture.' }
    $tuiTested = $false
    if ($TuiProbe) {
        $previousPreference = $ErrorActionPreference
        try {
            $ErrorActionPreference = 'Continue'
            $probeOutput = (& $TuiProbe $exe $workspace.FullName 2>&1 | Out-String)
            $probeExit = $LASTEXITCODE
        } finally {
            $ErrorActionPreference = $previousPreference
        }
        if ($probeExit -ne 0 -or $probeOutput -notmatch 'TUI_EDIT_FOLLOW_UP_PASS') {
            throw "Unexpected TUI smoke result $probeOutput"
        }
        if ([IO.File]::ReadAllText($file) -ne ($original -replace 'reject', 'allow')) {
            throw 'The approved TUI edit did not produce the expected bytes.'
        }
        $lineReview = (& $exe --workspace $workspace.FullName --model fixture-edit --session clean-p4 --write config.txt --resume | Out-String)
        if ($LASTEXITCODE -ne 0 -or $lineReview -notmatch '\[task 2\] Verify from TUI' -or
            $lineReview -notmatch 'end_turn requests=5 dispatches=3') {
            throw "The line CLI could not reopen the TUI session $lineReview"
        }
        $tuiTested = $true
    }
    $filesystem = (Get-CimInstance Win32_LogicalDisk -Filter "DeviceID='$env:SystemDrive'").FileSystem
    if ($filesystem -ne 'NTFS') { throw "Expected NTFS, observed $filesystem." }
    $result = [ordered]@{
        result='pass'; os=$os.Caption; version=$os.Version; architecture=$os.OSArchitecture
        filesystem=$filesystem; archive_sha256=$hash.Hash.ToLowerInvariant()
        executable_sha256=$exeHash.ToLowerInvariant(); started=$true; workspace_read=$true
        edit_requires_approval=$true; toolchain_commands_present=@()
    }
    if ($tuiTested) {
        $result.tui_edit_approved = $true
        $result.tui_follow_up = $true
        $result.shared_session_reopened = $true
    }
    if ($DualFixture) {
        if (!$TuiProbe) { throw 'Dual fixture requires the native TUI probe.' }
        $dual = (& $DualFixture -Application $exe -Probe $TuiProbe -Scratch $scratch | Out-String)
        if ($dual -notmatch 'DUAL_CLEAN_PASS') { throw "Dual-file guest smoke failed $dual" }
        $result.tui_two_files_verified = $true
        $result.cli_reopened_two_file_verification = $true
    }
    $json = $result | ConvertTo-Json -Compress
    if ($port) {
        $tag = if ($TuiProbe) { 'LAY_P7_RESULT ' } else { 'LAY_P4_RESULT ' }
        $port.WriteLine($tag + $json)
    }
    $json
} catch {
    if ($port) {
        $tag = if ($TuiProbe) { 'LAY_P7_ERROR ' } else { 'LAY_P4_ERROR ' }
        $port.WriteLine($tag + $_.Exception.Message)
    }
    throw
} finally {
    if ($port) { $port.Dispose() }
}
