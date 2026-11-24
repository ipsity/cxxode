param(
    [string]$Executable = (Join-Path $PSScriptRoot 'lay-code.exe'),
    [Parameter(Mandatory)][string]$Provider,
    [string]$Config = (Join-Path $env:LOCALAPPDATA 'LayCode/providers.json')
)

$ErrorActionPreference = 'Stop'
$app = (Resolve-Path -LiteralPath $Executable).Path
$file = (Resolve-Path -LiteralPath $Config).Path
[Console]::OutputEncoding = [Text.UTF8Encoding]::new($false)
$selection = @('--provider', $Provider, '--provider-config', $file)

Write-Host '第一步：在下面粘贴 API 密钥，然后按回车。输入时不会显示密钥。'
& $app --set-provider-key @selection
if ($LASTEXITCODE -ne 0) {
    throw '密钥未保存。'
}
$models = @(& $app --list-models @selection)
if ($LASTEXITCODE -ne 0 -or $models.Count -eq 0) {
    throw '未能读取模型列表。请核对密钥和服务商的模型访问权限，再运行此窗口。'
}
Write-Host "已获得 $($models.Count) 个模型。"
while ($true) {
    $filter = Read-Host '输入关键字筛选，直接回车显示前 30 个'
    $shown = @($models | Where-Object { $_.IndexOf($filter, [StringComparison]::OrdinalIgnoreCase) -ge 0 } |
        Select-Object -First 30)
    for ($i = 0; $i -lt $shown.Count; $i++) {
        Write-Host ("{0,2}. {1}" -f (1 + $i), $shown[$i])
    }
    $answer = Read-Host '输入编号或完整模型 ID。回车重新筛选'
    $number = 0
    if ([int]::TryParse($answer, [ref]$number) -and $number -ge 1 -and $number -le $shown.Count) {
        $model = $shown[$number - 1]
        break
    }
    if ($answer -and $models -ccontains $answer) {
        $model = $answer
        break
    }
}
$encoding = [Text.UTF8Encoding]::new($false)
$handle = [IO.File]::Open($file, [IO.FileMode]::Open, [IO.FileAccess]::ReadWrite, [IO.FileShare]::None)
try {
    if ($handle.Length -gt 65536) {
        throw '服务商配置不能超过 64 KiB。'
    }
    $reader = [IO.BinaryReader]::new($handle, $encoding, $true)
    try {
        $previous = $reader.ReadBytes([int]$handle.Length)
    } finally {
        $reader.Dispose()
    }
    $document = $encoding.GetString($previous).TrimStart([char]0xFEFF) | ConvertFrom-Json
    $profile = $document.providers.PSObject.Properties[$Provider]
    if (!$profile) {
        throw '配置文件中已经没有该服务商。'
    }
    $profile.Value | Add-Member -NotePropertyName model -NotePropertyValue $model -Force
    $updated = $encoding.GetBytes(($document | ConvertTo-Json -Depth 100) + [Environment]::NewLine)
    if ($updated.Length -gt 65536) {
        throw '保存后服务商配置会超过 64 KiB。'
    }
    try {
        $handle.Position = 0
        $handle.Write($updated, 0, $updated.Length)
        $handle.SetLength($updated.Length)
        $handle.Flush($true)
    } catch {
        $handle.Position = 0
        $handle.Write($previous, 0, $previous.Length)
        $handle.SetLength($previous.Length)
        $handle.Flush($true)
        throw
    }
} finally {
    $handle.Dispose()
}
Write-Host "已为 $Provider 保存默认模型 $model。"
