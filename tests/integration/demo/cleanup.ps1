<#
.SYNOPSIS
  Undoes prepare.ps1 and launch_agents.ps1: stops the demo tray, deletes the demo tokens and their token files.

.EXAMPLE
  .\cleanup.ps1                    # tray, tokens, token files
  .\cleanup.ps1 -CloseSessions     # also close the demo's SAP windows
#>
param(
    [string]$OutDir = '',
    [switch]$CloseSessions,
    [string]$Fairyfly = ''
)

$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'demo_common.ps1')
if (-not $OutDir) { $OutDir = $DemoDefaults.OutDir }
$exe = Resolve-DemoExe $Fairyfly
$manifestPath = Join-Path $OutDir 'agents.json'
$manifest = if (Test-Path -LiteralPath $manifestPath) { Get-Content -LiteralPath $manifestPath -Raw | ConvertFrom-Json }

if ($manifest -and $manifest.tray_pid) {
    $proc = Get-Process -Id $manifest.tray_pid -ErrorAction SilentlyContinue
    if ($proc -and $proc.ProcessName -eq 'fairyfly') { Stop-Process -Id $proc.Id -Force; Write-Host "tray $($proc.Id) stopped" }
}

$names = if ($manifest) { @($manifest.agents | ForEach-Object { $_.token_name }) }
         else { @(1..$DemoScenarios.Count | ForEach-Object { "$($DemoDefaults.TokenStem)$_" }) }
foreach ($name in $names) {
    $deleted = Invoke-DemoFairyfly $exe @('mcp', 'token', 'delete', $name, '--yes')
    Write-Host ("token {0}: {1}" -f $name, $(if ($deleted.status -eq 'success') { 'deleted' } else { 'not found' }))
}
Get-ChildItem -LiteralPath $OutDir -Filter token.txt -Recurse -ErrorAction SilentlyContinue | Remove-Item -Force
Remove-Item -LiteralPath (Join-Path $OutDir 'go.signal') -ErrorAction SilentlyContinue

if ($CloseSessions) {
    # Pre-opened windows are in agents.json, agent-opened ones in launched.json (written by launch_agents.ps1).
    $connections = @()
    if ($manifest) { $connections += @($manifest.agents | Where-Object { $null -ne $_.connection } | ForEach-Object { $_.connection }) }
    $launchedFile = Join-Path $OutDir 'launched.json'
    if (Test-Path -LiteralPath $launchedFile) {
        $connections += @(Get-Content -LiteralPath $launchedFile -Raw | ConvertFrom-Json | ForEach-Object { $_.connection })
    }
    foreach ($id in ($connections | Select-Object -Unique)) {
        $closed = Invoke-DemoFairyfly $exe @('session', 'disconnect', '--connection', "$id", '--close-session')
        Write-Host ("SAP window of connection {0}: {1}" -f $id, $closed.status)
    }
    Remove-Item -LiteralPath $launchedFile -ErrorAction SilentlyContinue
}
Write-Host 'Demo cleanup done.'
