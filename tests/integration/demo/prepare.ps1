<#
.SYNOPSIS
  Prepares the README demo: one token per agent and the fairyfly tray (HTTP MCP); optionally the SAP GUI windows.

.DESCRIPTION
  Needs SAP GUI with scripting enabled, the SAP Logon entry -Entry and a stored credential of the same name
  (fairyfly credentials set Bigfox --user DEVELOPER --client 001). Creates one read-only token per agent (written to
  <OutDir>\agentN\token.txt, never printed) and starts `fairyfly mcp --http --tray` in read-only guard mode with an
  owner allowlist. By default the agents open their own SAP windows (launch_agents.ps1 places each one above its
  agent). With -PreOpenWindows it instead opens one window per agent now (`session launch --login`, 30-45 s each), saves them
  as connections, resets them to the start screen and tiles them into the top row. Writes <OutDir>\agents.json for
  launch_agents.ps1. Undo with cleanup.ps1.

.EXAMPLE
  .\prepare.ps1                   # tokens and tray; the agents open their SAP windows
  .\prepare.ps1 -PreOpenWindows   # also open and tile the SAP windows now
  .\prepare.ps1 -WhatIf           # show the plan only
#>
[CmdletBinding(SupportsShouldProcess = $true)]
param(
    [int]$Port = 0,
    [string]$OutDir = '',
    [string]$Entry = '',
    [string]$Identity = '',
    [string]$Fairyfly = '',
    [switch]$PreOpenWindows
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'demo_common.ps1')
if (-not $Port) { $Port = $DemoDefaults.Port }
if (-not $OutDir) { $OutDir = $DemoDefaults.OutDir }
if (-not $Entry) { $Entry = $DemoDefaults.Entry }
if (-not $Identity) { $Identity = $DemoDefaults.Identity }
$exe = Resolve-DemoExe $Fairyfly
$count = $DemoScenarios.Count
$system = ($Identity -split '/')[0..1] -join '/'
$cfgPath = Join-Path $OutDir 'demo.yaml'

Write-Host "fairyfly:  $exe"
Write-Host "endpoint:  http://127.0.0.1:$Port/mcp (read-only guard mode, owner $Identity)"
Write-Host "windows:   $count x SAP Logon entry '$Entry'"
Write-Host "out dir:   $OutDir"
if (-not $PSCmdlet.ShouldProcess('SAP GUI, Credential Manager and tray', 'prepare the demo')) { return }

if (Test-DemoPort $Port) { throw "Port $Port is in use (a running tray?). Run cleanup.ps1 or choose -Port." }
if (-not (Start-DemoSapLogon)) { throw 'SAP Logon (saplogon.exe) is not running and could not be started' }
New-Item -ItemType Directory -Force -Path $OutDir | Out-Null

$agents = @()
foreach ($scenario in $DemoScenarios) {
    $agents += [pscustomobject]@{
        agent = $scenario.Agent; tcode = $scenario.Tcode; prompt = $scenario.Prompt
        session_id = $null; connection = $null
        title = "agent $($scenario.Agent) $([char]0x00B7) $($scenario.Tcode)"   # middle dot; scripts stay ASCII for PowerShell 5.1
        token_name = "$($DemoDefaults.TokenStem)$($scenario.Agent)"
    }
}

if ($PreOpenWindows) {
    # 1. One logged-on SAP GUI session per agent. Only sessions whose main window really exists count: a connection that SAP
    #    has just closed can still show up in the session list for a moment.
    function Get-LiveSessions {
        @(Get-DemoSessionIds (Invoke-DemoFairyfly $exe @('session', 'list')) |
            Where-Object { (Get-DemoSapWindowHandle $_) -ne [IntPtr]::Zero })
    }
    $sessions = Get-LiveSessions
    while ($sessions.Count -lt $count) {
        Write-Host "opening SAP window $($sessions.Count + 1) of $count (session launch $Entry --login) ..."
        $launched = Invoke-DemoFairyfly $exe @('session', 'launch', $Entry, '--login', '--multiple-logon', 'keep')
        if ($launched.status -ne 'success') {
            throw "session launch failed: $($launched.error.code) $($launched.error.message)"
        }
        $sessions = Get-LiveSessions
    }
    $sessions = @($sessions | Select-Object -First $count)

    # 2. Save each window as a fairyfly connection, go to the start screen, tile it into the top row.
    Invoke-DemoFairyfly $exe @('connection', 'list', '--cleanup') | Out-Null
    for ($i = 0; $i -lt $count; $i++) {
        $attached = Invoke-DemoFairyfly $exe @('session', 'attach', '--session-id', $sessions[$i])
        if ($attached.status -ne 'success') { throw "attach $($sessions[$i]) failed: $($attached.error.code)" }
        $agents[$i].connection = [int]$attached.data.connection_file_id
        $agents[$i].session_id = $sessions[$i]
        Invoke-DemoFairyfly $exe @('transaction', 'start', '/n', '--connection', "$($agents[$i].connection)") | Out-Null
        $handle = Get-DemoSapWindowHandle $sessions[$i]
        if (-not (Set-DemoWindowCell $handle (Get-DemoCell $i 0))) { Write-Warning "could not place the SAP window of $($sessions[$i])" }
        Write-Host ("window {0}: {1} -> connection {2} ({3})" -f ($i + 1), $sessions[$i], $agents[$i].connection, $agents[$i].tcode)
    }
} else {
    Write-Host 'SAP windows: opened by the agents themselves (gui_session_launch + gui_session_login)'
}

# 3. One read-only token per agent (the sticky connection and the rate budget are per token).
foreach ($a in $agents) {
    & $exe mcp token delete $a.token_name --yes --output json 2>$null | Out-Null   # leftover of an earlier run
    $issued = Invoke-DemoFairyfly $exe @('mcp', 'token', 'create', $a.token_name,
        '--scope', 'session,session.lease,connection,screen,element,transaction,key,popup',
        # --system is the SID only (any client): a window on its logon screen reports client 000, and a launch is refused
        # while an open window of the same SAP Logon entry is on a system outside the list. The owner allowlist still
        # admits only $Identity.
        '--sap-identity', $Identity, '--system', ($Identity -split '/')[0], '--connections', $Entry,
        '--read-only', '--expires', '1d', '--yes')
    if ($issued.status -ne 'success' -or -not $issued.data.token) { throw "token $($a.token_name) could not be created" }
    $dir = Join-Path $OutDir "agent$($a.agent)"
    New-Item -ItemType Directory -Force -Path $dir | Out-Null
    Set-Content -LiteralPath (Join-Path $dir 'token.txt') -Value $issued.data.token -NoNewline -Encoding ascii
}

# 4. The tray: HTTP MCP on loopback, read-only guard mode, owner allowlist.
@('server:', '  transport: http', '  host: 127.0.0.1', "  port: $Port",
  'owner:', "  sap_identities: [$Identity]") | Set-Content -LiteralPath $cfgPath -Encoding ascii
# Start the tray process directly as the detached tray child (what `mcp --tray` does from a console), so this works
# from consoles and console-less hosts alike and the pid is known.
# The audit trail must be on: launch_agents.ps1 finds the agents' SAP windows through it.
$env:FAIRYFLY_TRAY_CHILD = '1'
$auditSetting = $env:FAIRYFLY_AUDIT
Remove-Item Env:FAIRYFLY_AUDIT -ErrorAction SilentlyContinue
try {
    $tray = Start-Process -FilePath $exe -ArgumentList @('mcp', '--http', '--tray', '--config', "`"$cfgPath`"") `
        -WindowStyle Hidden -PassThru
} finally {
    Remove-Item Env:FAIRYFLY_TRAY_CHILD -ErrorAction SilentlyContinue
    if ($null -ne $auditSetting) { $env:FAIRYFLY_AUDIT = $auditSetting }
}
$trayPid = $tray.Id
$ready = $false
for ($attempt = 0; $attempt -lt 50 -and -not $ready; $attempt++) {
    $ready = Test-DemoPort $Port
    if (-not $ready) { Start-Sleep -Milliseconds 200 }
}
if (-not $ready) { throw 'The tray did not start listening' }

[pscustomobject]@{
    endpoint = "http://127.0.0.1:$Port/mcp"; tray_pid = $trayPid; config = $cfgPath
    identity = $Identity; agents = $agents
} | ConvertTo-Json -Depth 5 | Set-Content -LiteralPath (Join-Path $OutDir 'agents.json') -Encoding utf8

Write-Host ''
Write-Host "Ready: tray pid $trayPid on http://127.0.0.1:$Port/mcp, $count windows, $count tokens."
Write-Host 'Next: .\launch_agents.ps1 -NoStart (check the layout), then start the VirtualBox recording and run .\launch_agents.ps1'
