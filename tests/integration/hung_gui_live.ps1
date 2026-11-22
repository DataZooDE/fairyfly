<#
.SYNOPSIS
  Live check of issue #155 (hung SAP GUI: detection, no false alarms, guarded restart) through `fairyfly mcp`.

.DESCRIPTION
  Speaks newline-delimited JSON-RPC to two `fairyfly mcp` servers on this desktop:

    server 1: mcp --allow-write                       (automatic SAP startup OFF)
      responsive.f3_changes_screen   SU01 then F3: success with screen_changed true, never SAP_GUI_UNRESPONSIVE
      responsive.no_false_alarm      three SU01/F3 rounds: no SAP_GUI_UNRESPONSIVE
      inactive_keys.no_false_alarm   F12 three times on SAP Easy Access (an inactive key is a SAP error, not a no-op)
      restart.refused_without_flag   gui_session_restart -> SAP_AUTOSTART_DISABLED, saplogon.exe keeps its PID
    server 2: mcp --allow-write --allow-sap-startup    (automatic SAP startup ON)
      restart.ends_and_prepares      gui_session_restart -> sap_gui_restarted, a new saplogon.exe PID
      restart.session_usable         gui_screen_read after the restart answers a SAP screen
      restart.navigation_after       SU01 then F3 works on the restarted GUI

  Not covered live: the automatic trip (three acknowledged no-op navigations on an idle session). No reliable way
  was found to put a real SAP GUI into that state on purpose (2026-11-22: a disabled main window, a SAP modal
  wnd[1], a native dialog opened by a scripting call, which blocks every COM call instead, and the About dialog of
  SAP Logon all left SAP GUI reacting or failing loudly). The trip, soft recovery and the automatic restart are
  covered by tests/unit/test_mcp_hang.cpp and use the same restart code as gui_session_restart.

  Prerequisites: SAP Logon running with ONE logged-on session of the local default connection (`fairyfly session
  default`) on SAP Easy Access, its credential in Windows Credential Manager (`fairyfly credentials list`), no other
  work open in SAP GUI: server 2 ENDS SAP GUI (all its sessions) and logs on again.
  Required screen and transaction: SAP Easy Access, no popup. Only navigates (SU01 initial screen, F3, F12).

  Exit code: 0 all checks passed, 1 at least one check failed, 2 no SAP session.

.PARAMETER Exe
  Path to fairyfly.exe (default: build\Release\fairyfly.exe under the repo root).
#>
[CmdletBinding()]
param([string]$Exe = '')

$ErrorActionPreference = 'Stop'
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $Exe) { $Exe = Join-Path $RepoRoot 'build\Release\fairyfly.exe' }
if (-not (Test-Path -LiteralPath $Exe)) { Write-Host "fairyfly.exe not found: $Exe"; exit 1 }

function Start-Server([string]$Arguments) {
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = $Arguments
    $psi.RedirectStandardInput = $true
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.UseShellExecute = $false
    $psi.CreateNoWindow = $true
    $psi.StandardOutputEncoding = [Text.Encoding]::UTF8
    foreach ($name in @('FAIRYFLY_READ_ONLY')) { if ($psi.EnvironmentVariables.ContainsKey($name)) { $psi.EnvironmentVariables.Remove($name) } }
    $psi.EnvironmentVariables['FAIRYFLY_AUDIT'] = '0'
    $proc = [System.Diagnostics.Process]::Start($psi)
    $proc.BeginErrorReadLine()
    $srv = @{ Proc = $proc; Pending = $null; NextId = 1; Eof = $false }
    [void](Invoke-Rpc $srv 'initialize' @{ protocolVersion = '2025-06-18'; capabilities = @{}; clientInfo = @{ name = 'hung-gui-live'; version = '1.0' } })
    $proc.StandardInput.WriteLine('{"jsonrpc":"2.0","method":"notifications/initialized"}')
    $proc.StandardInput.Flush()
    return $srv
}
function Read-ServerLine($Srv, [int]$TimeoutMs) {
    if ($Srv.Eof) { return $null }
    if ($null -eq $Srv.Pending) { $Srv.Pending = $Srv.Proc.StandardOutput.ReadLineAsync() }
    if (-not $Srv.Pending.Wait($TimeoutMs)) { return $null }
    $line = $Srv.Pending.Result
    $Srv.Pending = $null
    if ($null -eq $line) { $Srv.Eof = $true }
    return $line
}
function Invoke-Rpc($Srv, [string]$Method, $Params = $null, [int]$TimeoutMs = 180000) {
    $id = $Srv.NextId; $Srv.NextId = $id + 1
    $msg = [ordered]@{ jsonrpc = '2.0'; id = $id; method = $Method }
    if ($null -ne $Params) { $msg['params'] = $Params }
    $Srv.Proc.StandardInput.WriteLine(($msg | ConvertTo-Json -Compress -Depth 20))
    $Srv.Proc.StandardInput.Flush()
    while ($true) {
        $line = Read-ServerLine $Srv $TimeoutMs
        if ($null -eq $line) { return $null }
        try { $obj = $line | ConvertFrom-Json } catch { continue }
        if ($null -ne $obj.PSObject.Properties['id'] -and $obj.id -eq $id) { return $obj }
    }
}
function Invoke-Tool($Srv, [string]$Name, $Arguments = @{}) { return Invoke-Rpc $Srv 'tools/call' @{ name = $Name; arguments = $Arguments } }
function Stop-Server($Srv) {
    if ($null -eq $Srv) { return }
    try { $Srv.Proc.StandardInput.Close() } catch { }
    if (-not $Srv.Proc.WaitForExit(20000)) { try { $Srv.Proc.Kill() } catch { } }
}
function Get-Text($R) {
    if ($null -eq $R -or $null -eq $R.PSObject.Properties['result']) { return '' }
    return (@($R.result.content) | Where-Object { $_.type -eq 'text' } | ForEach-Object { [string]$_.text }) -join "`n"
}
function Get-ErrCode($R) { if ((Get-Text $R) -match '^ERROR ([A-Z0-9_]+):') { return $Matches[1] }; return '' }
function Test-IsError($R) { return ($null -ne $R -and $null -ne $R.PSObject.Properties['result'] -and $R.result.isError -eq $true) }
function Get-Structured($R) { if ($null -ne $R -and $null -ne $R.result.PSObject.Properties['structuredContent']) { return $R.result.structuredContent }; return $null }
function Get-SapLogonPids { return @(Get-Process saplogon -ErrorAction SilentlyContinue | ForEach-Object { $_.Id } | Sort-Object) }

$script:Pass = 0; $script:Fail = 0
function Check([string]$Name, [scriptblock]$Body) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    try { [void](& $Body); $script:Pass++; Write-Host ('PASS {0} [{1}ms]' -f $Name, $sw.ElapsedMilliseconds) }
    catch { $script:Fail++; Write-Host ('FAIL {0} [{1}ms] - {2}' -f $Name, $sw.ElapsedMilliseconds, $_.Exception.Message) }
}
function Assert-That($Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Assert-NoHangAlarm($R, [string]$What) {
    Assert-That ((Get-ErrCode $R) -ne 'SAP_GUI_UNRESPONSIVE' -and (Get-ErrCode $R) -ne 'SAP_GUI_RECOVERED') "$What raised $(Get-ErrCode $R): $(Get-Text $R)"
}
function Invoke-Su01AndBack($Srv) {
    $t = Invoke-Tool $Srv 'gui_transaction_start' @{ code = '/nSU01' }
    Assert-That (-not (Test-IsError $t)) "SU01 did not start: $(Get-Text $t)"
    $k = Invoke-Tool $Srv 'gui_key_send' @{ key = 'F3' }
    Assert-NoHangAlarm $k 'F3'
    Assert-That (-not (Test-IsError $k)) "F3 failed: $(Get-Text $k)"
    return $k
}

$s1 = $null; $s2 = $null
$exit = 0
try {
    if ((Get-SapLogonPids).Count -ne 1) { Write-Host 'exactly one saplogon.exe must run'; exit 2 }
    $s1 = Start-Server 'mcp --allow-write'
    $list = Invoke-Tool $s1 'gui_session_list'
    if ((Get-Text $list) -notmatch 'ses\[0\]') { Write-Host 'no SAP session'; exit 2 }

    Check 'responsive.f3_changes_screen' {
        $k = Invoke-Su01AndBack $s1
        $st = Get-Structured $k
        Assert-That ($null -ne $st -and $st.data.screen_changed -eq $true) "F3 did not report screen_changed true: $(Get-Text $k)"
    }
    Check 'responsive.no_false_alarm' { for ($i = 0; $i -lt 3; $i++) { [void](Invoke-Su01AndBack $s1) } }
    Check 'inactive_keys.no_false_alarm' {
        [void](Invoke-Tool $s1 'gui_transaction_start' @{ code = '/n' })
        for ($i = 0; $i -lt 3; $i++) { Assert-NoHangAlarm (Invoke-Tool $s1 'gui_key_send' @{ key = 'F12' }) 'F12 on SAP Easy Access' }
    }
    Check 'restart.refused_without_flag' {
        $before = Get-SapLogonPids
        $r = Invoke-Tool $s1 'gui_session_restart'
        Assert-That ((Get-ErrCode $r) -eq 'SAP_AUTOSTART_DISABLED') "expected SAP_AUTOSTART_DISABLED, got: $(Get-Text $r)"
        Assert-That (((Get-SapLogonPids) -join ',') -eq ($before -join ',')) 'saplogon.exe changed although the restart was refused'
    }
    Stop-Server $s1; $s1 = $null

    $s2 = Start-Server 'mcp --allow-write --allow-sap-startup'
    [void](Invoke-Tool $s2 'gui_session_list')
    Check 'restart.ends_and_prepares' {
        $before = Get-SapLogonPids
        $r = Invoke-Tool $s2 'gui_session_restart'
        Assert-That (-not (Test-IsError $r)) "restart failed: $(Get-Text $r)"
        $st = Get-Structured $r
        Assert-That ($st.data.sap_gui_restarted -eq $true -and $st.data.unsaved_input_lost -eq $true) "result lacks the restart fields: $(Get-Text $r)"
        $after = Get-SapLogonPids
        Assert-That ($after.Count -eq 1 -and $before -notcontains $after[0]) "saplogon.exe was not restarted (before $($before -join ','), after $($after -join ','))"
    }
    Check 'restart.session_usable' {
        $r = Invoke-Tool $s2 'gui_screen_read' @{ only = 'editable' }
        Assert-That (-not (Test-IsError $r)) "screen read after the restart failed: $(Get-Text $r)"
    }
    Check 'restart.navigation_after' { [void](Invoke-Su01AndBack $s2) }
} finally {
    Stop-Server $s1
    Stop-Server $s2
}
Write-Host ('{0} passed, {1} failed' -f $script:Pass, $script:Fail)
if ($script:Fail -gt 0) { $exit = 1 }
exit $exit
