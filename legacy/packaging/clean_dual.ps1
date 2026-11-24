param(
    [Parameter(Mandatory=$true)][string]$Application,
    [Parameter(Mandatory=$true)][string]$Probe,
    [Parameter(Mandatory=$true)][string]$Scratch
)

$ErrorActionPreference = 'Stop'
$workspace = (New-Item -ItemType Directory -Path (Join-Path $Scratch 'dual-workspace')).FullName
$profile = Join-Path $Scratch 'dual-provider.json'
$ready = Join-Path $Scratch 'dual-relay-ready.txt'
$utf8 = [Text.UTF8Encoding]::new($false)
$first = 'function Get-PageStart([int]$Number, [int]$Size) { return $Number * $Size }' + [char]10
$checkLine = 'if ((Get-PageStart 1 2) -lt 0) { exit 1 }'
$oldCheck = '. (Join-Path $PSScriptRoot ''pager.ps1'')' + [char]10 + $checkLine + [char]10
$newCheck = 'if ((Get-PageStart 1 2) -ne 0 -or (Get-PageStart 2 2) -ne 2) { exit 1 }'
[IO.File]::WriteAllText((Join-Path $workspace 'pager.ps1'), $first, $utf8)
[IO.File]::WriteAllText((Join-Path $workspace 'pager-check.ps1'), $oldCheck, $utf8)

$probePort = [Net.Sockets.TcpListener]::new([Net.IPAddress]::Loopback, 0)
$probePort.Start()
$port = $probePort.LocalEndpoint.Port
$probePort.Stop()
$endpoint = "http://127.0.0.1:$port"
$job = Start-Job -ArgumentList $endpoint,$ready,$checkLine,$newCheck -ScriptBlock {
    param($Address, $ReadyPath, $BeforeCheck, $AfterCheck)
    $ErrorActionPreference = 'Stop'
    function New-Call($Id, $Name, $Arguments) {
        @{type='function_call'; id="fc_$Id"; call_id=$Id; name=$Name
          arguments=($Arguments | ConvertTo-Json -Compress -Depth 8)}
    }
    function VersionOf($Output) {
        $match = [regex]::Match($Output, '(?m)^version=(\S+)$')
        if (!$match.Success) { throw 'Read version is missing' }
        $match.Groups[1].Value
    }
    $server = [Net.HttpListener]::new()
    $server.Prefixes.Add("$Address/")
    try {
        $server.Start()
        [IO.File]::WriteAllText($ReadyPath, 'ready')
        for ($i = 0; $i -lt 4; ++$i) {
            $client = $server.GetContext()
            $reader = [IO.StreamReader]::new($client.Request.InputStream)
            try { $request = $reader.ReadToEnd() | ConvertFrom-Json } finally { $reader.Dispose() }
            $results = @{}
            foreach ($item in $request.input) {
                if ($item.type -eq 'function_call_output') { $results[$item.call_id] = $item.output }
            }
            switch ($i) {
                0 {
                    $pieces = @(
                        (New-Call 'read-source' 'read' @{path='pager.ps1'})
                        (New-Call 'read-check' 'read' @{path='pager-check.ps1'})
                    )
                }
                1 {
                    $pieces = @(
                        (New-Call 'edit-source' 'edit' @{path='pager.ps1'; version=(VersionOf $results['read-source'])
                            old_text='$Number * $Size'; new_text='($Number - 1) * $Size'})
                        (New-Call 'edit-check' 'edit' @{path='pager-check.ps1'; version=(VersionOf $results['read-check'])
                            old_text=$BeforeCheck; new_text=$AfterCheck})
                    )
                }
                2 {
                    $pieces = @(New-Call 'verify' 'run' @{
                        program="$env:SystemRoot/System32/WindowsPowerShell/v1.0/powershell.exe"
                        args=@('-NoProfile','-ExecutionPolicy','Bypass','-File','pager-check.ps1')
                    })
                }
                3 {
                    $pieces = @(@{type='message'; id='msg_dual'; role='assistant'; content=@(
                        @{type='output_text'; text='Both file actions and the regression result are recorded.'}
                    )})
                }
            }
            $frame = @{type='response.completed'; response=@{status='completed'; output=$pieces}}
            $body = [Text.Encoding]::UTF8.GetBytes('data: ' + ($frame | ConvertTo-Json -Compress -Depth 12) +
                [char]10 + [char]10)
            $client.Response.ContentType = 'text/event-stream'
            $client.Response.ContentLength64 = $body.Length
            $client.Response.OutputStream.Write($body, 0, $body.Length)
            $client.Response.Close()
        }
    } finally {
        $server.Close()
    }
}
try {
    $limit = [DateTime]::UtcNow.AddSeconds(12)
    while (!(Test-Path -LiteralPath $ready) -and $job.State -eq 'Running' -and [DateTime]::UtcNow -lt $limit) {
        Start-Sleep -Milliseconds 100
    }
    if (!(Test-Path -LiteralPath $ready)) { throw ("Clean guest relay did not start. " + (Receive-Job $job | Out-String)) }
    $definition = @{default_provider='dual'; providers=@{dual=@{protocol='responses'; model='pager-fixture'
        base_url="$endpoint/v1"; api_key_env=$null}}}
    [IO.File]::WriteAllText($profile, ($definition | ConvertTo-Json -Depth 8), $utf8)
    $evidence = (& $Probe $Application $workspace --two-files $profile | Out-String)
    if ($LASTEXITCODE -ne 0 -or $evidence -notmatch 'TUI_TWO_FILE_VERIFICATION_PASS') { throw "Dual TUI failed $evidence" }
    $arguments = '--workspace', $workspace, '--session', 'clean-dual', '--write', 'pager-check.ps1',
        '--write', 'pager.ps1', '--allow-run', '--provider-config', $profile, '--resume'
    $line = (& $Application @arguments | Out-String)
    if ($LASTEXITCODE -ne 0 -or $line -notmatch 'pager\.ps1 \[applied\]' -or
        $line -notmatch 'pager-check\.ps1 \[applied\]' -or $line -notmatch 'command exited 0 at edit revision 2' -or
        $line -notmatch 'end_turn requests=4 dispatches=5') {
        throw "Dual CLI inspection failed $line"
    }
    if ([IO.File]::ReadAllText((Join-Path $workspace 'pager.ps1')) -ne
        $first.Replace('$Number * $Size', '($Number - 1) * $Size') -or
        [IO.File]::ReadAllText((Join-Path $workspace 'pager-check.ps1')) -ne $oldCheck.Replace($checkLine, $newCheck)) {
        throw 'Dual file bytes differ from the reviewed edits'
    }
    'DUAL_CLEAN_PASS'
} finally {
    Stop-Job -Job $job
    Remove-Job -Job $job
}
