<#
.SYNOPSIS
  Opens one Claude Code agent per demo scenario for the README demo, tiled into the bottom row of the screen.

.DESCRIPTION
  Run prepare.ps1 first. For each agent it writes <OutDir>\agentN\ (mcp.json for the HTTP MCP endpoint with that
  agent's token, CLAUDE.md with its connection id and the lease rule, start.ps1), opens a Windows Terminal window
  with the "fairyfly demo" profile (Cascadia Mono, logo colours, focus mode, fixed title "agent N - TCODE"), places it
  below its SAP window and, once all are placed, starts the agents at the same moment.

  The first start in a new folder asks Claude Code's one-time "trust this folder" question: answer it during the
  rehearsal so the recorded run shows none.

.EXAMPLE
  .\launch_agents.ps1 -NoStart     # windows and layout only (check fonts), no agents
  .\launch_agents.ps1              # the real run
#>
param(
    [string]$OutDir = '',
    [switch]$NoStart,
    [string]$Model = 'sonnet',
    [int]$FontSize = 12,
    [int]$WatchMinutes = 6
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'demo_common.ps1')
if (-not $OutDir) { $OutDir = $DemoDefaults.OutDir }
$manifestPath = Join-Path $OutDir 'agents.json'
if (-not (Test-Path -LiteralPath $manifestPath)) { throw "No ${manifestPath}: run prepare.ps1 first" }
$manifest = Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json
if (-not (Get-Command wt.exe -ErrorAction SilentlyContinue)) {
    throw 'Windows Terminal (wt.exe) is missing: winget install --id Microsoft.WindowsTerminal -e'
}
if (-not $NoStart -and -not (Get-Command claude -ErrorAction SilentlyContinue)) { throw 'Claude Code (claude) is not on PATH' }
# The agents' first gui_session_launch binds the tray to SAP GUI: SAP Logon has to run by then (see Start-DemoSapLogon).
if (-not $NoStart -and -not (Start-DemoSapLogon)) { throw 'SAP Logon (saplogon.exe) is not running and could not be started' }

# Windows Terminal profile and colour scheme as a JSON fragment (no change to the user's settings.json).
$fragmentDir = Join-Path $env:LOCALAPPDATA 'Microsoft\Windows Terminal\Fragments\fairyfly'
New-Item -ItemType Directory -Force -Path $fragmentDir | Out-Null
@{
    profiles = @(@{
        name = 'fairyfly demo'; commandline = 'powershell.exe -NoLogo'; colorScheme = 'fairyfly'
        font = @{ face = 'Cascadia Mono'; size = $FontSize }; padding = '12'; scrollbarState = 'hidden'
        cursorShape = 'bar'; suppressApplicationTitle = $true; bellStyle = 'none'; antialiasingMode = 'cleartype'
    })
    schemes = @(@{
        name = 'fairyfly'; background = '#0A111A'; foreground = '#E2E8F0'; cursorColor = '#3CD6B2'
        selectionBackground = '#1C3144'
        black = '#0A111A'; red = '#E5484D'; green = '#25B493'; yellow = '#E0A800'; blue = '#4C8DFF'
        purple = '#B07CFF'; cyan = '#3CD6B2'; white = '#E2E8F0'
        brightBlack = '#5B6B7F'; brightRed = '#FF6369'; brightGreen = '#3CD6B2'; brightYellow = '#F5C542'
        brightBlue = '#7AA8FF'; brightPurple = '#C9A2FF'; brightCyan = '#7EF0D4'; brightWhite = '#FFFFFF'
    })
} | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $fragmentDir 'fairyfly-demo.json') -Encoding utf8

$go = Join-Path $OutDir 'go.signal'
Remove-Item -LiteralPath $go -ErrorAction SilentlyContinue

foreach ($a in $manifest.agents) {
    $dir = Join-Path $OutDir "agent$($a.agent)"
    if (-not (Test-Path -LiteralPath (Join-Path $dir 'token.txt'))) { throw "Missing token for agent $($a.agent): run prepare.ps1" }
    @{ mcpServers = @{ fairyfly = @{
        type = 'http'; url = $manifest.endpoint; headers = @{ Authorization = 'Bearer ${FAIRYFLY_TOKEN}' }
    } } } | ConvertTo-Json -Depth 6 | Set-Content -LiteralPath (Join-Path $dir 'mcp.json') -Encoding utf8
    $windowRules = if ($null -ne $a.connection) {
        @"
You operate SAP GUI only through the fairyfly MCP tools. Your SAP window is saved connection $($a.connection).
Other agents work in other SAP windows at the same time: never use another connection.

- Pass ``connection: $($a.connection)`` on every fairyfly tool call.
"@
    } else {
        @"
You operate SAP GUI only through the fairyfly MCP tools. Other agents work in their own SAP windows at the same time:
open your own window first and never touch another one.

- Open your SAP window: ``gui_session_launch`` with ``name: "$($DemoDefaults.Entry)"`` and ``login: false``. It
  returns ``connection_file_id``; that is your connection. Then log on with ``gui_session_login``
  (``connection``: that id, ``credential: "$($DemoDefaults.Entry)"``, ``multiple_logon: "keep"``).
- Pass ``connection`` (your id) on every later fairyfly tool call.
"@
    }
    @"
# Demo agent $($a.agent)

$windowRules
- Before navigating (transaction start, clicks, keys), acquire a lease with ``gui_session_lease``
  (``action: acquire``) and pass its ``lease_id`` on those calls. Renew it if a task takes longer than 45 seconds and
  release it when you are done.
- Read screens with ``gui_screen_read`` and ``no_tabs: true``; keep ``max_rows`` small.
- The server is read-only: do not try to change data.
- Answer in at most five short bullet points.
"@ | Set-Content -LiteralPath (Join-Path $dir 'CLAUDE.md') -Encoding utf8
    $prompt = $a.prompt.Replace("'", "''")
    $claude = if ($NoStart) {
        "Write-Host 'Ready. (layout check: -NoStart)' -ForegroundColor DarkGray"
    } else {
        "while (-not (Test-Path -LiteralPath '$go')) { Start-Sleep -Milliseconds 100 }`r`n" +
        # The prompt goes first: --allowedTools and --mcp-config take several values and would swallow it.
        "claude '$prompt' --model=$Model --mcp-config=mcp.json --strict-mcp-config --allowedTools=mcp__fairyfly"
    }
    @"
# Markers of a parent Claude Code session (when this script is started from one) would change the agents' behaviour
# and footer; the agents must start like a fresh terminal.
Get-ChildItem Env: | Where-Object { `$_.Name -like 'CLAUDE_CODE*' -or `$_.Name -eq 'CLAUDECODE' } |
    ForEach-Object { Remove-Item -LiteralPath ("Env:" + `$_.Name) }
`$env:FAIRYFLY_TOKEN = (Get-Content -LiteralPath (Join-Path `$PSScriptRoot 'token.txt') -Raw).Trim()
Clear-Host
Write-Host ''
Write-Host '  $($a.title)' -ForegroundColor Cyan -NoNewline
Write-Host '   $(if ($null -ne $a.connection) { "SAP window: connection $($a.connection)" } else { "opens its own SAP window" })' -ForegroundColor DarkGray
Write-Host ''
$claude
"@ | Set-Content -LiteralPath (Join-Path $dir 'start.ps1') -Encoding utf8

    $wtArgs = "-w new --focus --title `"$($a.title)`" --suppressApplicationTitle -p `"fairyfly demo`" -d `"$dir`" " +
              "powershell.exe -NoLogo -NoExit -ExecutionPolicy Bypass -File `"$(Join-Path $dir 'start.ps1')`""
    Start-Process -FilePath 'wt.exe' -ArgumentList $wtArgs
}

# Pre-opened SAP windows (prepare.ps1 -PreOpenWindows): reset each to its start screen (a rehearsal leaves them on the
# last transaction) and re-tile them into the top row (the screen may have changed). Then place each terminal below.
$exe = Resolve-DemoExe ''
for ($i = 0; $i -lt $manifest.agents.Count; $i++) {
    if ($null -eq $manifest.agents[$i].connection) { continue }
    Invoke-DemoFairyfly $exe @('transaction', 'start', '/n', '--connection', "$($manifest.agents[$i].connection)") | Out-Null
    $sap = Get-DemoSapWindowHandle $manifest.agents[$i].session_id
    if (-not (Set-DemoWindowCell $sap (Get-DemoCell $i 0))) { Write-Warning "could not place SAP window $($manifest.agents[$i].session_id)" }
}
for ($i = 0; $i -lt $manifest.agents.Count; $i++) {
    $a = $manifest.agents[$i]
    $handle = [IntPtr]::Zero
    for ($try = 0; $try -lt 50 -and $handle -eq [IntPtr]::Zero; $try++) {
        $handle = [DemoWin32]::FindByTitle($a.title)
        if ($handle -eq [IntPtr]::Zero) { Start-Sleep -Milliseconds 200 }
    }
    if (-not (Set-DemoWindowCell $handle (Get-DemoCell $i 1))) { Write-Warning "could not place '$($a.title)'" }
}

if ($NoStart) {
    Write-Host 'Layout ready (no agents started). Close the agent terminals, then run without -NoStart.'
} else {
    # Agent-opened windows: the MCP audit trail records each successful gui_session_launch with the token name and the
    # saved connection, which leads to the SAP session and its window. Remember where the file ends before the start.
    $auditFile = if ($env:FAIRYFLY_AUDIT_FILE) { $env:FAIRYFLY_AUDIT_FILE }
                 else { Join-Path $env:LOCALAPPDATA ('fairyfly\audit\' + (Get-Date).ToUniversalTime().ToString('yyyy-MM') + '.jsonl') }
    $offset = if (Test-Path -LiteralPath $auditFile) { (Get-Item -LiteralPath $auditFile).Length } else { 0 }
    Remove-Item -LiteralPath (Join-Path $OutDir 'launched.json') -ErrorAction SilentlyContinue
    $pending = @{}
    for ($i = 0; $i -lt $manifest.agents.Count; $i++) {
        if ($null -eq $manifest.agents[$i].connection) { $pending[$manifest.agents[$i].token_name] = $i }
    }

    # A new token reaches the running tray within a few seconds: start the agents only when every token authenticates.
    $ready = @{}
    for ($try = 0; $try -lt 40 -and $ready.Count -lt $manifest.agents.Count; $try++) {
        foreach ($a in $manifest.agents) {
            if ($ready.ContainsKey($a.token_name)) { continue }
            $token = (Get-Content -LiteralPath (Join-Path $OutDir "agent$($a.agent)\token.txt") -Raw).Trim()
            try {
                $reply = Invoke-RestMethod -Method Post -Uri $manifest.endpoint -ContentType 'application/json' -TimeoutSec 10 `
                    -Headers @{ Authorization = "Bearer $token"; Accept = 'application/json' } `
                    -Body '{"jsonrpc":"2.0","id":1,"method":"tools/list"}'
                if ($reply.result.tools) { $ready[$a.token_name] = $true }
            } catch { }
        }
        if ($ready.Count -lt $manifest.agents.Count) { Start-Sleep -Milliseconds 500 }
    }
    if ($ready.Count -lt $manifest.agents.Count) { throw 'Not every agent token authenticates at the endpoint: rerun prepare.ps1' }

    Start-Sleep -Milliseconds 500
    New-Item -ItemType File -Path $go -Force | Out-Null   # all agents start now
    Write-Host 'Agents started.'

    $deadline = (Get-Date).AddMinutes($WatchMinutes)
    while ($pending.Count -gt 0 -and (Get-Date) -lt $deadline) {
        Start-Sleep -Milliseconds 300
        if (-not (Test-Path -LiteralPath $auditFile)) { continue }
        $lines = @()
        try {
            $stream = [IO.File]::Open($auditFile, 'Open', 'Read', 'ReadWrite')
            if ($stream.Length -gt $offset) {
                [void]$stream.Seek($offset, 'Begin')
                $text = (New-Object IO.StreamReader($stream)).ReadToEnd()
                $complete = $text.LastIndexOf("`n")
                if ($complete -ge 0) {
                    $lines = $text.Substring(0, $complete).Split("`n")
                    $offset += [Text.Encoding]::UTF8.GetByteCount($text.Substring(0, $complete + 1))
                }
            }
            $stream.Dispose()
        } catch { continue }
        foreach ($line in $lines) {
            if ($line -notmatch '"gui_session_launch"') { continue }
            $record = try { $line | ConvertFrom-Json } catch { $null }
            if (-not $record -or $record.tool -ne 'gui_session_launch' -or $record.status -ne 'success' -or
                $null -eq $record.connection -or -not $pending.ContainsKey([string]$record.principal)) { continue }
            $column = $pending[[string]$record.principal]
            $saved = @((Invoke-DemoFairyfly $exe @('connection', 'list')).data.connections |
                Where-Object { $_.id -eq $record.connection })[0]
            $sap = if ($saved) { Get-DemoSapWindowHandle $saved.session_id } else { [IntPtr]::Zero }
            if ($saved) {   # for cleanup.ps1 -CloseSessions
                $launchedFile = Join-Path $OutDir 'launched.json'
                $known = @(if (Test-Path -LiteralPath $launchedFile) { Get-Content -LiteralPath $launchedFile -Raw | ConvertFrom-Json })
                @($known) + [pscustomobject]@{ token = [string]$record.principal; connection = $record.connection; session_id = $saved.session_id } |
                    ConvertTo-Json -Depth 3 | Set-Content -LiteralPath $launchedFile -Encoding utf8
            }
            if (Set-DemoWindowCell $sap (Get-DemoCell $column 0)) {
                Write-Host ("{0}: SAP window {1} (connection {2}) placed" -f $record.principal, $saved.session_id, $record.connection)
                $pending.Remove([string]$record.principal)
            } else {
                Write-Warning "$($record.principal): could not place the window of connection $($record.connection)"
                $pending.Remove([string]$record.principal)
            }
        }
    }
    if ($pending.Count) { Write-Warning ("no SAP window placed for: " + (($pending.Keys | Sort-Object) -join ', ')) }
}
