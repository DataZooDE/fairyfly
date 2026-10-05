# Shared helpers of the README demo scripts (prepare.ps1, launch_agents.ps1, cleanup.ps1). Dot-source it.

$DemoDefaults = @{
    Port      = 8383
    OutDir    = Join-Path $env:LOCALAPPDATA 'fairyfly-demo'
    Identity  = 'A4H/001/DEVELOPER'
    Entry     = 'Bigfox'
    TokenStem = 'demo-agent'
}

# The demo scenarios, one agent and one screen column each: read-only Basis tasks that work without typing (see tests/integration/BASIS_SMOKE_TESTS.md).
# SM37 stays on the default selection (own user): an all-users SM37 search once froze SAP GUI scripting for minutes.
$DemoScenarios = @(
    # SM04 rather than ST22: dump lists change from day to day (often empty), the user list always shows the demo's own sessions.
    @{ Agent = 1; Tcode = 'SM04'; Prompt = 'Open SM04 and tell me which users are logged on, with how many sessions and from which terminals.' },
    @{ Agent = 2; Tcode = 'SM50'; Prompt = 'Open SM50 and tell me which work processes are busy right now and what they are doing.' },
    @{ Agent = 3; Tcode = 'SM59'; Prompt = 'Open SM59 and list the RFC destinations under ABAP Connections with their descriptions, without opening each one.' }
    # A fourth scenario (one column each, so fewer columns means wider windows):
    # @{ Agent = 4; Tcode = 'SM37'; Prompt = 'Open SM37, run the default selection for my own jobs and summarize their status.' }
)

function Resolve-DemoExe([string]$Fairyfly) {
    if (-not $Fairyfly) { $Fairyfly = Join-Path $PSScriptRoot '..\..\..\build\bin\Release\fairyfly.exe' }
    if (-not (Test-Path -LiteralPath $Fairyfly)) { $Fairyfly = Join-Path $PSScriptRoot '..\..\..\build\Release\fairyfly.exe' }
    return (Resolve-Path -LiteralPath $Fairyfly).Path
}

# Runs fairyfly with JSON output and returns the parsed envelope; never prints secrets.
function Invoke-DemoFairyfly([string]$Exe, [string[]]$Arguments) {
    $raw = & $Exe @Arguments --output json 2>$null
    $text = ($raw | Out-String).Trim()
    if (-not $text) { return [pscustomobject]@{ status = 'error'; error = @{ code = 'NO_OUTPUT' } } }
    try { return $text | ConvertFrom-Json } catch { throw "fairyfly $($Arguments -join ' ') returned no JSON" }
}

# Every live SAP GUI session id ("/app/con[0]/ses[0]") found anywhere in a session list result.
function Get-DemoSessionIds($ListResult) {
    $ids = [System.Collections.Generic.List[string]]::new()
    function Walk($node) {
        if ($null -eq $node) { return }
        if ($node -is [string]) {
            if ($node -match '^/app/con\[\d+\]/ses\[\d+\]$' -and -not $ids.Contains($node)) { $ids.Add($node) }
            return
        }
        if ($node -is [System.Collections.IEnumerable] -and -not ($node -is [string])) {
            foreach ($item in $node) { Walk $item }
            return
        }
        if ($node -is [pscustomobject]) { foreach ($p in $node.PSObject.Properties) { Walk $p.Value } }
    }
    Walk $ListResult.data
    return @($ids | Sort-Object { [int]([regex]::Match($_, 'con\[(\d+)\]').Groups[1].Value) },
                                { [int]([regex]::Match($_, 'ses\[(\d+)\]').Groups[1].Value) })
}

function Test-DemoPort([int]$Port) {
    try {
        $tcp = [Net.Sockets.TcpClient]::new()
        $tcp.Connect('127.0.0.1', $Port)
        $tcp.Dispose()
        return $true
    } catch { return $false }
}

# Screen grid: one column per scenario x 2 rows over the primary screen's working area (SAP windows on top, agents below).
function Get-DemoCell([int]$Column, [int]$Row) {
    Add-Type -AssemblyName System.Windows.Forms
    $area = [System.Windows.Forms.Screen]::PrimaryScreen.WorkingArea
    $w = [int][Math]::Floor($area.Width / $DemoScenarios.Count)
    $h = [int][Math]::Floor($area.Height / 2)
    return @{ X = $area.X + $Column * $w; Y = $area.Y + $Row * $h; W = $w; H = $h }
}

if (-not ('DemoWin32' -as [type])) {
    Add-Type @'
using System;
using System.Runtime.InteropServices;
using System.Text;
public static class DemoWin32 {
    public delegate bool EnumProc(IntPtr hwnd, IntPtr lparam);
    [DllImport("user32.dll")] public static extern bool EnumWindows(EnumProc cb, IntPtr lparam);
    [DllImport("user32.dll", CharSet = CharSet.Unicode)] public static extern int GetWindowText(IntPtr h, StringBuilder s, int n);
    [DllImport("user32.dll")] public static extern bool IsWindowVisible(IntPtr h);
    [DllImport("user32.dll")] public static extern bool ShowWindow(IntPtr h, int cmd);
    [DllImport("user32.dll")] public static extern bool SetWindowPos(IntPtr h, IntPtr after, int x, int y, int cx, int cy, uint flags);
    public static IntPtr FindByTitle(string title) {
        IntPtr found = IntPtr.Zero;
        EnumWindows((h, l) => {
            if (!IsWindowVisible(h)) return true;
            var sb = new StringBuilder(512);
            GetWindowText(h, sb, sb.Capacity);
            if (sb.ToString() == title) { found = h; return false; }
            return true;
        }, IntPtr.Zero);
        return found;
    }
}
'@
}

# Window handle of the main window of a SAP GUI session, read through the scripting API (read-only). Uses VBScript:
# its late binding needs no type library, which PowerShell's COM adapter fails to load (TYPE_E_CANTLOADLIBRARY).
function Get-DemoSapWindowHandle([string]$SessionId) {
    $vbs = Join-Path $env:TEMP 'fairyfly_demo_hwnd.vbs'
    @'
Set app = GetObject("SAPGUI").GetScriptingEngine
WScript.Echo app.FindById(WScript.Arguments(0) & "/wnd[0]").Handle
'@ | Set-Content -LiteralPath $vbs -Encoding ascii
    $out = ''
    try {
        $previous = $ErrorActionPreference; $ErrorActionPreference = 'Continue'   # cscript's stderr must not abort
        $out = (& cscript.exe //NoLogo $vbs $SessionId 2>$null | Out-String).Trim()
    } catch { $out = '' } finally { $ErrorActionPreference = $previous }
    if ($out -notmatch '^-?\d+$') { return [IntPtr]::Zero }
    return [IntPtr][long]$out
}

# SAP Logon must run before the tray starts: without it SAP GUI scripting falls back to an embedded
# SapGui.ScriptingCtrl inside the tray process, whose windows the owner-mode session workers cannot reach
# (gui_session_list then fails with OWNER_IDENTITY_UNKNOWN and the agents never get control of their windows).
function Start-DemoSapLogon {
    if (Get-Process saplogon -ErrorAction SilentlyContinue) { return $true }
    $candidates = @("$env:ProgramFiles\SAP\FrontEnd\SAPGUI\saplogon.exe", "${env:ProgramFiles(x86)}\SAP\FrontEnd\SAPGUI\saplogon.exe")
    $path = $candidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
    if (-not $path) { return $false }
    Start-Process -FilePath $path -WindowStyle Minimized
    for ($i = 0; $i -lt 50; $i++) {
        if (Get-Process saplogon -ErrorAction SilentlyContinue) { Start-Sleep -Seconds 3; return $true }
        Start-Sleep -Milliseconds 200
    }
    return $false
}

function Set-DemoWindowCell([IntPtr]$Handle, [hashtable]$Cell) {
    if ($Handle -eq [IntPtr]::Zero) { return $false }
    [void][DemoWin32]::ShowWindow($Handle, 9)   # SW_RESTORE: un-maximize first
    return [DemoWin32]::SetWindowPos($Handle, [IntPtr]::Zero, $Cell.X, $Cell.Y, $Cell.W, $Cell.H, 0x0040)   # SWP_SHOWWINDOW
}
