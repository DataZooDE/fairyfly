<#
.SYNOPSIS
  Live test of the cloud link (`fairyfly link run`, D10) against a real fairyfly gateway and a logged-in SAP GUI.

.DESCRIPTION
  Runs `fairyfly link run` from this checkout's build, then acts as the AI assistant: it calls the gateway's MCP
  endpoint with the access token you pass, and checks:
    1. `link status` says this desktop is paired, and the running link reports "connected";
    2. tools/list through the gateway lists fairyfly's tools (the catalog of this desktop);
    3. a read call (gui_screen_read) reaches SAP through the gateway and comes back;
    4. the local audit trail has that call with audit_source "link", the gateway's request id and the principal
       link:<organisation>/<user>, and holds no screen content;
    5. `link pause` disconnects: within a few seconds the status is "paused" and the gateway answers DEVICE_OFFLINE;
    6. `link resume` reconnects, and the read call works again;
    7. after the link process ends, the status says "not running".

  Prerequisites:
    - Windows PowerShell 5.1, SAP GUI with scripting enabled, a logged-in session (SAP Easy Access, no popup);
    - this desktop paired once by a person: `fairyfly link pair` (asks, https://fairyfly.io prefilled) (QR + number on the phone);
    - no other `fairyfly link run` on this desktop (it uses the real pairing and its pause marker; exit 2 otherwise);
    - an access token for the gateway's MCP endpoint of the same user, e.g. from the web app's connector wizard
      ("Run test" shows how) or from your AI platform's OAuth flow. Pass it in the environment variable
      FFY_ACCESS_TOKEN; it is never printed or written anywhere.

  What it touches in SAP: reads the current screen only. It never fills a field, presses a button or starts a
  transaction. A temporary audit file (FAIRYFLY_AUDIT_FILE) keeps the user's real audit trail out of it.

  Exit code: 0 all checks passed, 1 at least one check failed, 2 prerequisite missing.

.PARAMETER Exe
  Path to fairyfly.exe (default: build\Release\fairyfly.exe, then build\bin\Release\fairyfly.exe under the repo root).
.PARAMETER Gateway
  Web address of the gateway (default: the environment variable FAIRYFLY_LINK_GATEWAY).
.PARAMETER DryRun
  Print the plan and exit.
#>
[CmdletBinding()]
param(
    [string]$Exe = '',
    [string]$Gateway = $env:FAIRYFLY_LINK_GATEWAY,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$script:failures = 0
function Pass([string]$what) { Write-Host "PASS  $what" -ForegroundColor Green }
function Fail([string]$what) { Write-Host "FAIL  $what" -ForegroundColor Red; $script:failures++ }
function Check([bool]$ok, [string]$what) { if ($ok) { Pass $what } else { Fail $what } }

$root = Resolve-Path (Join-Path $PSScriptRoot '..\..')
if (-not $Exe) {
    foreach ($c in @('build\Release\fairyfly.exe', 'build\bin\Release\fairyfly.exe')) {
        $p = Join-Path $root $c
        if (Test-Path $p) { $Exe = $p; break }
    }
}
if ($DryRun) {
    Write-Host "Plan: $Exe link status; link run (temp audit file); MCP initialize, tools/list, tools/call gui_screen_read via $Gateway/mcp;"
    Write-Host "      audit check; link pause -> DEVICE_OFFLINE; link resume -> call again; stop -> not running."
    exit 0
}
if (-not $Exe -or -not (Test-Path $Exe)) { Write-Host "fairyfly.exe not found; build it or pass -Exe"; exit 2 }
if (-not $Gateway) { Write-Host "Pass -Gateway or set FAIRYFLY_LINK_GATEWAY"; exit 2 }
if (-not $env:FFY_ACCESS_TOKEN) { Write-Host "Set FFY_ACCESS_TOKEN to an access token for $Gateway/mcp"; exit 2 }
$Gateway = $Gateway.TrimEnd('/')

function Ffy([string[]]$argv) {
    $out = & $Exe @argv --output json 2>$null
    if (-not $out) { return $null }
    return ($out | Out-String | ConvertFrom-Json)
}

$status = Ffy @('link', 'status')
if (-not $status -or -not $status.data.paired) { Write-Host "This desktop is not paired: run fairyfly link pair --gateway $Gateway"; exit 2 }
Pass "paired as $($status.data.account) in $($status.data.organisation)"
# a second link for the same device would compete with the running one, and the pause below would disconnect it too
if ($status.data.link -ne 'not running') {
    Write-Host "A fairyfly link is already running here ($($status.data.link)); stop it first, then run this test"
    exit 2
}
if ($status.data.paused) { & $Exe link resume | Out-Null }

$rpcId = 0
function Mcp([string]$method, $params) {
    $script:rpcId++
    $body = @{ jsonrpc = '2.0'; id = $script:rpcId; method = $method; params = $params } | ConvertTo-Json -Depth 10 -Compress
    $headers = @{ Authorization = "Bearer $($env:FFY_ACCESS_TOKEN)"; Accept = 'application/json, text/event-stream' }
    $r = Invoke-WebRequest -UseBasicParsing -Method Post -Uri "$Gateway/mcp" -Headers $headers -ContentType 'application/json' -Body $body
    return ($r.Content | ConvertFrom-Json)
}
function ScreenRead() { return Mcp 'tools/call' @{ name = 'gui_screen_read'; arguments = @{} } }
function ErrorText($res) { if ($res.result -and $res.result.isError) { return ($res.result.content | ForEach-Object { $_.text }) -join ' ' } return '' }
function WaitFor([scriptblock]$cond, [int]$seconds = 20) {
    $deadline = (Get-Date).AddSeconds($seconds)
    while ((Get-Date) -lt $deadline) { if (& $cond) { return $true }; Start-Sleep -Milliseconds 500 }
    return (& $cond)
}

$audit = Join-Path ([IO.Path]::GetTempPath()) ("ffy-link-live-" + [Guid]::NewGuid().ToString('N') + '.jsonl')
$env:FAIRYFLY_AUDIT_FILE = $audit
$proc = $null
try {
    $proc = Start-Process -FilePath $Exe -ArgumentList @('link', 'run') -PassThru -WindowStyle Hidden
    Check (WaitFor { (Ffy @('link', 'status')).data.link -eq 'connected' }) "the running link reports connected"

    $init = Mcp 'initialize' @{ protocolVersion = '2025-06-18'; capabilities = @{}; clientInfo = @{ name = 'link_live.ps1'; version = '1' } }
    Check ($null -ne $init.result) "MCP initialize through the gateway"
    $ok = WaitFor { $l = Mcp 'tools/list' @{}; $null -ne ($l.result.tools | Where-Object { $_.name -eq 'gui_screen_read' }) }
    Check $ok "tools/list through the gateway lists this desktop's tools"

    $read = ScreenRead
    Check ($read.result -and -not $read.result.isError) "gui_screen_read reaches SAP through the gateway ($(ErrorText $read))"

    Start-Sleep -Seconds 1
    $records = @(Get-Content $audit -ErrorAction SilentlyContinue | ForEach-Object { $_ | ConvertFrom-Json } |
        Where-Object { $_.audit_source -eq 'link' -and $_.tool -eq 'gui_screen_read' })
    Check ($records.Count -ge 1) "local audit has the call with audit_source link"
    if ($records.Count -ge 1) {
        $rec = $records[-1]
        Check ([bool]$rec.request_id) "audit record carries the gateway's request id"
        Check ("$($rec.principal)" -like 'link:*/*') "audit principal is link:<organisation>/<user>"
        $screenText = ($read.result.content | ForEach-Object { $_.text }) -join ''
        $line = Get-Content $audit -Raw
        Check (-not ($screenText.Length -gt 40 -and $line.Contains($screenText.Substring(0, 40)))) "audit holds no screen content"
    }

    & $Exe link pause | Out-Null
    Check (WaitFor { (Ffy @('link', 'status')).data.link -eq 'paused' }) "link pause: status paused"
    Check (WaitFor { (ErrorText (ScreenRead)) -match 'DEVICE_OFFLINE' } 30) "paused: the gateway answers DEVICE_OFFLINE"

    & $Exe link resume | Out-Null
    Check (WaitFor { (Ffy @('link', 'status')).data.link -eq 'connected' }) "link resume: status connected"
    Check (WaitFor { $r = ScreenRead; $r.result -and -not $r.result.isError } 30) "resumed: the read call works again"
} catch {
    Fail "unexpected error: $($_.Exception.Message)"
} finally {
    if ($proc -and -not $proc.HasExited) { Stop-Process -Id $proc.Id -Force; $proc.WaitForExit(10000) | Out-Null }
    & $Exe link resume | Out-Null
    Remove-Item Env:FAIRYFLY_AUDIT_FILE -ErrorAction SilentlyContinue
    Remove-Item $audit -ErrorAction SilentlyContinue
}
# a killed process cannot write "stopped": its pid is gone, so the status reads "not running" at once
Check (WaitFor { (Ffy @('link', 'status')).data.link -eq 'not running' } 5) "after the link ends: not running (at once)"
Check ($null -ne (Ffy @('link', 'status')).data.last_seen) "after the link ends: last_seen keeps what it last wrote"

if ($script:failures -gt 0) { Write-Host "$script:failures check(s) failed"; exit 1 }
Write-Host "all checks passed"
exit 0
