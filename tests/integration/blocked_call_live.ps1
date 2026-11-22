<#
.SYNOPSIS
  Live check of issue #155, second variant: a SAP GUI scripting call that does not return, through `fairyfly mcp`.

.DESCRIPTION
  Two real blocking situations on this desktop, each answered at the call deadline (--call-timeout-ms 15000):

    server 1: mcp --allow-write --call-timeout-ms 15000                       (automatic SAP startup OFF)
      server_stall.answer            SE37 test of ENQUE_SLEEP (45 s): F8 blocks in SAP; at the deadline
                                     SAP_SERVER_NOT_RESPONDING (SAP GUI still reacts), saplogon.exe keeps its PID
      server_stall.recovers          once the server step ends, calls work again
      frozen.no_permission           saplogon.exe suspended: SAP_GUI_NOT_RESPONDING, restart.reason
                                     SAP_AUTOSTART_DISABLED, saplogon.exe keeps its PID (resumed afterwards)
    server 2: mcp --allow-write --allow-sap-startup --call-timeout-ms 15000   (automatic SAP startup ON)
      frozen.restarted               saplogon.exe suspended: SAP_GUI_RESTARTED, the suspended saplogon.exe is gone
      frozen.session_prepared        the next call prepares the default session again (new saplogon.exe PID)

  Prerequisites: SAP Logon running with ONE logged-on session of the local default connection (`fairyfly session
  default`) on SAP Easy Access, its credential in Windows Credential Manager, nobody working in SAP GUI: server 2
  ENDS SAP GUI. Required transaction: SE37 (display/test of the standard function module ENQUE_SLEEP, which only
  waits). Suspending saplogon.exe is how a frozen frontend is produced on purpose (its windows stop answering).

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

Add-Type @"
using System; using System.Runtime.InteropServices;
public static class Freeze {
  [DllImport("kernel32.dll")] static extern IntPtr OpenProcess(uint access, bool inherit, int pid);
  [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr h);
  [DllImport("ntdll.dll")] static extern int NtSuspendProcess(IntPtr h);
  [DllImport("ntdll.dll")] static extern int NtResumeProcess(IntPtr h);
  public static bool Suspend(int pid) { var h = OpenProcess(0x800, false, pid); if (h == IntPtr.Zero) return false; var r = NtSuspendProcess(h); CloseHandle(h); return r == 0; }
  public static bool Resume(int pid) { var h = OpenProcess(0x800, false, pid); if (h == IntPtr.Zero) return false; var r = NtResumeProcess(h); CloseHandle(h); return r == 0; }
}
"@

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
    if ($psi.EnvironmentVariables.ContainsKey('FAIRYFLY_READ_ONLY')) { $psi.EnvironmentVariables.Remove('FAIRYFLY_READ_ONLY') }
    $psi.EnvironmentVariables['FAIRYFLY_AUDIT'] = '0'
    $proc = [System.Diagnostics.Process]::Start($psi)
    $proc.BeginErrorReadLine()
    $srv = @{ Proc = $proc; Pending = $null; NextId = 1; Eof = $false }
    [void](Invoke-Rpc $srv 'initialize' @{ protocolVersion = '2025-06-18'; capabilities = @{}; clientInfo = @{ name = 'blocked-call-live'; version = '1.0' } })
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
    if (-not $Srv.Proc.WaitForExit(30000)) { try { $Srv.Proc.Kill() } catch { } }
}
function Get-Text($R) {
    if ($null -eq $R -or $null -eq $R.PSObject.Properties['result']) { return '' }
    return (@($R.result.content) | Where-Object { $_.type -eq 'text' } | ForEach-Object { [string]$_.text }) -join "`n"
}
function Get-ErrCode($R) { if ((Get-Text $R) -match '^(?:ERROR )?([A-Z][A-Z0-9_]+):') { return $Matches[1] }; return '' }
function Test-IsError($R) { return ($null -ne $R -and $null -ne $R.PSObject.Properties['result'] -and $R.result.isError -eq $true) }
function Get-ErrorObject($R) { if ($null -ne $R -and $null -ne $R.result.PSObject.Properties['structuredContent']) { return $R.result.structuredContent.error }; return $null }
function Get-SapLogonPids { return @(Get-Process saplogon -ErrorAction SilentlyContinue | ForEach-Object { $_.Id } | Sort-Object) }
# Calls are refused with SERVER_BUSY while a blocked call still runs: retry until the server serves again.
function Invoke-WhenFree($Srv, [string]$Name, $Arguments = @{}, [int]$Seconds = 120) {
    $until = (Get-Date).AddSeconds($Seconds)
    while ($true) {
        $r = Invoke-Tool $Srv $Name $Arguments
        if ((Get-ErrCode $r) -ne 'SERVER_BUSY' -or (Get-Date) -gt $until) { return $r }
        Start-Sleep -Milliseconds 1000
    }
}

$script:Pass = 0; $script:Fail = 0
function Check([string]$Name, [scriptblock]$Body) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    try { [void](& $Body); $script:Pass++; Write-Host ('PASS {0} [{1}ms]' -f $Name, $sw.ElapsedMilliseconds) }
    catch { $script:Fail++; Write-Host ('FAIL {0} [{1}ms] - {2}' -f $Name, $sw.ElapsedMilliseconds, $_.Exception.Message) }
}
function Assert-That($Condition, [string]$Message) { if (-not $Condition) { throw $Message } }

$s1 = $null; $s2 = $null; $script:suspended = 0
try {
    if ((Get-SapLogonPids).Count -ne 1) { Write-Host 'exactly one saplogon.exe must run'; exit 2 }
    $s1 = Start-Server 'mcp --allow-write --call-timeout-ms 15000'
    if ((Get-Text (Invoke-Tool $s1 'gui_session_list')) -notmatch 'ses\[0\]') { Write-Host 'no SAP session'; exit 2 }

    Check 'server_stall.answer' {
        $before = Get-SapLogonPids
        Assert-That (-not (Test-IsError (Invoke-Tool $s1 'gui_transaction_start' @{ code = '/nSE37' }))) 'SE37 did not start'
        [void](Invoke-Tool $s1 'gui_element_fill' @{ element = 'wnd[0]/usr/ctxtRS38L-NAME'; value = 'ENQUE_SLEEP' })
        [void](Invoke-Tool $s1 'gui_key_send' @{ key = 'F8' })
        [void](Invoke-Tool $s1 'gui_element_fill' @{ element = 'wnd[0]/usr/txt[34,7]'; value = '45' })
        $r = Invoke-Tool $s1 'gui_key_send' @{ key = 'F8' }   # the server sleeps 45 s; the deadline is 15 s
        Assert-That ((Get-ErrCode $r) -eq 'SAP_SERVER_NOT_RESPONDING') "expected SAP_SERVER_NOT_RESPONDING, got: $(Get-Text $r)"
        Assert-That (((Get-SapLogonPids) -join ',') -eq ($before -join ',')) 'saplogon.exe changed during a server stall'
    }
    Check 'server_stall.recovers' {
        $r = Invoke-WhenFree $s1 'gui_transaction_start' @{ code = '/n' }
        Assert-That (-not (Test-IsError $r)) "no recovery after the server step: $(Get-Text $r)"
    }
    Check 'frozen.no_permission' {
        $before = Get-SapLogonPids
        Assert-That ([Freeze]::Suspend($before[0])) 'cannot suspend saplogon.exe'
        $script:suspended = $before[0]
        try {
            $r = Invoke-Tool $s1 'gui_screen_read' @{ only = 'editable' }
            $e = Get-ErrorObject $r
            Assert-That ((Get-ErrCode $r) -eq 'SAP_GUI_NOT_RESPONDING') "expected SAP_GUI_NOT_RESPONDING, got: $(Get-Text $r)"
            Assert-That ($e.restart.reason -eq 'SAP_AUTOSTART_DISABLED') "restart reason: $($e.restart.reason)"
            Assert-That (((Get-SapLogonPids) -join ',') -eq ($before -join ',')) 'saplogon.exe was ended without permission'
        } finally {
            [void][Freeze]::Resume($script:suspended); $script:suspended = 0
        }
        $after = Invoke-WhenFree $s1 'gui_session_list'
        Assert-That (-not (Test-IsError $after)) "no recovery after resuming: $(Get-Text $after)"
    }
    Stop-Server $s1; $s1 = $null

    $s2 = Start-Server 'mcp --allow-write --allow-sap-startup --call-timeout-ms 15000'
    [void](Invoke-Tool $s2 'gui_session_list')   # the watch needs to know which SAP Logon entries are open
    $script:frozenPid = 0
    Check 'frozen.restarted' {
        $before = Get-SapLogonPids
        $script:frozenPid = $before[0]
        $frozenPid = $script:frozenPid
        Assert-That ([Freeze]::Suspend($frozenPid)) 'cannot suspend saplogon.exe'
        $script:suspended = $frozenPid
        try {
            $r = Invoke-Tool $s2 'gui_screen_read' @{ only = 'editable' }
            $e = Get-ErrorObject $r
            Assert-That ((Get-ErrCode $r) -eq 'SAP_GUI_RESTARTED') "expected SAP_GUI_RESTARTED, got: $(Get-Text $r)"
            Assert-That ($e.sap_gui_restarted -eq $true -and $e.unsaved_input_lost -eq $true) 'restart fields missing'
            Assert-That (-not (Get-Process -Id $frozenPid -ErrorAction SilentlyContinue)) 'the frozen saplogon.exe still runs'
        } finally {
            if (Get-Process -Id $frozenPid -ErrorAction SilentlyContinue) { [void][Freeze]::Resume($frozenPid) }
            $script:suspended = 0
        }
    }
    Check 'frozen.session_prepared' {
        $r = Invoke-WhenFree $s2 'gui_screen_read' @{ only = 'editable' }
        Assert-That (-not (Test-IsError $r)) "the next call did not prepare the session: $(Get-Text $r)"
        $after = Get-SapLogonPids
        Assert-That ($after.Count -eq 1 -and $after[0] -ne $script:frozenPid) "no new saplogon.exe (now $($after -join ','))"
    }
} finally {
    if ($script:suspended) { [void][Freeze]::Resume($script:suspended) }
    Stop-Server $s1
    Stop-Server $s2
}
Write-Host ('{0} passed, {1} failed' -f $script:Pass, $script:Fail)
if ($script:Fail -gt 0) { exit 1 }
exit 0
