<#
.SYNOPSIS
  Live check of issue #158 (SAP GUI gaps seen while writing an ABAP report in SE38), through `fairyfly mcp`.

.DESCRIPTION
  One `mcp --allow-write` server drives SE38 on the attached SAP session:

    abap.fill_change_mode     gui_element_fill on the ABAP editor (GuiShell/AbapEditor) replaces the source and answers
                              it read back (before: EXCEPTION "Failed to set property")
    abap.long_source          a 251-line source (more than one 200-line read page) is verified completely
    abap.saved                Save + Activate, the program opened again shows the written source
    abap.display_mode         in display mode the fill is refused with ELEMENT_READ_ONLY and the source stays (the
                              control itself accepts text there, which SAP would discard)
    combo.verified            Goto > Attributes: the program type combo box (cmbTRDIR-SUBC) filled with its displayed
                              text answers key "1" (before: ok with key "" and display_value "")
    menu.trailing_dots        gui_menu_select "Utilities/Settings..." matches "Settings" (before: MENU_ITEM_NOT_FOUND)
    tabs.nested_restore       gui_screen_read on the "User-Specific Settings" popup (nested tabstrip under the ABAP
                              Editor tab) succeeds and leaves both strips on their tab (before: TAB_RESTORE_FAILED with
                              no data, the popup left on the last tab)
    radio.selected            filling the "Text-Based Editor" radio answers selected:true and its caption (before:
                              value "" and no selected); the original choice is selected again, the popup cancelled
    menu.not_found_context    an unknown menu entry names the failing segment, the entries there and the screen
    find.text_contains        gui_screen_find text_contains finds a button by its visible text (before: INVALID_ARGUMENT)

  Prerequisites: SAP Logon with ONE logged-on session (the server attaches to it), SAP GUI scripting enabled, nobody
  working in that session. Required transaction: SE38. The check creates the local program ZFFY_FILL_TEST ($TMP, not
  transported) when it does not exist and overwrites, saves and activates its source on every run. Popups are always
  left with F12; the user-specific editor settings are never saved.

  Exit code: 0 all checks passed, 1 at least one check failed, 2 no SAP session / setup failed.

.PARAMETER Exe
  Path to fairyfly.exe (default: build\Release\fairyfly.exe under the repo root).
#>
[CmdletBinding()]
param([string]$Exe = '')

$ErrorActionPreference = 'Stop'
$RepoRoot = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
if (-not $Exe) { $Exe = Join-Path $RepoRoot 'build\Release\fairyfly.exe' }
if (-not (Test-Path -LiteralPath $Exe)) { Write-Host "fairyfly.exe not found: $Exe"; exit 1 }

$Program = 'ZFFY_FILL_TEST'
$Editor = 'wnd[0]/usr/cntlEDITOR/shellcont/shell'
$SettingsEditTab = 'wnd[1]/usr/tabsG_TABSTRIP/tabp0100/ssubTOOLAREA:SAPLWB_CUSTOMIZING:0100/tabsG_ED_TABSTR/tabpEDIT'
$SettingsRadios = "$SettingsEditTab/ssubEDIT:SAPLWB_CUSTOMIZING:0130"

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
    [void](Invoke-Rpc $srv 'initialize' @{ protocolVersion = '2025-06-18'; capabilities = @{}; clientInfo = @{ name = 'sap-gui-gaps-live'; version = '1.0' } })
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
function Get-Structured($R) { if ($null -ne $R -and $null -ne $R.result.PSObject.Properties['structuredContent']) { return $R.result.structuredContent }; return $null }
function Get-Data($R) { $s = Get-Structured $R; if ($null -ne $s) { return $s.data }; return $null }
function Get-ErrorObject($R) { $s = Get-Structured $R; if ($null -ne $s) { return $s.error }; return $null }

# Direct SAP GUI scripting (read only here): what a strip shows as selected, which window is on top.
function Invoke-Com($Object, [string]$Member, [string]$Kind, [object[]]$Arguments = @()) {
    return [System.__ComObject].InvokeMember($Member, $Kind, $null, $Object, $Arguments)
}
function Get-SapSession {
    $rot = [System.Runtime.InteropServices.Marshal]::BindToMoniker('SAPGUI')
    $app = Invoke-Com $rot 'GetScriptingEngine' 'InvokeMethod'
    return Invoke-Com $app 'FindById' 'InvokeMethod' @('/app/con[0]/ses[0]')
}
function Get-SelectedTabName([string]$StripId) {
    $strip = Invoke-Com (Get-SapSession) 'FindById' 'InvokeMethod' @($StripId)
    return [string](Invoke-Com (Invoke-Com $strip 'SelectedTab' 'GetProperty') 'Name' 'GetProperty')
}
function Test-PopupOpen { try { [void](Invoke-Com (Get-SapSession) 'FindById' 'InvokeMethod' @('wnd[1]')); return $true } catch { return $false } }
function Close-Popups($Srv) { for ($i = 0; $i -lt 3 -and (Test-PopupOpen); $i++) { [void](Invoke-Tool $Srv 'gui_key_send' @{ key = 'F12' }) } }

$script:Pass = 0; $script:Fail = 0
function Check([string]$Name, [scriptblock]$Body) {
    $sw = [Diagnostics.Stopwatch]::StartNew()
    try { [void](& $Body); $script:Pass++; Write-Host ('PASS {0} [{1}ms]' -f $Name, $sw.ElapsedMilliseconds) }
    catch { $script:Fail++; Write-Host ('FAIL {0} [{1}ms] - {2}' -f $Name, $sw.ElapsedMilliseconds, $_.Exception.Message) }
}
function Assert-That($Condition, [string]$Message) { if (-not $Condition) { throw $Message } }
function Assert-Ok($R, [string]$What) { Assert-That (-not (Test-IsError $R) -and $null -ne $R) "$What failed: $(Get-Text $R)" }

# SE38 with ZFFY_FILL_TEST in change mode (created as a local program when it is missing).
function Open-ProgramInChangeMode($Srv) {
    Close-Popups $Srv
    Assert-Ok (Invoke-Tool $Srv 'gui_transaction_start' @{ code = '/nSE38' }) 'SE38'
    Assert-Ok (Invoke-Tool $Srv 'gui_element_fill' @{ element = 'wnd[0]/usr/ctxtRS38M-PROGRAMM'; value = $Program }) 'program name'
    $r = Invoke-Tool $Srv 'gui_key_send' @{ key = '6' }   # Change
    if ((Get-Text $r) -match 'does not exist') {
        Assert-Ok (Invoke-Tool $Srv 'gui_key_send' @{ key = '5' }) 'Create'
        Assert-Ok (Invoke-Tool $Srv 'gui_element_fill' @{ element = 'wnd[1]/usr/txtRS38M-REPTI'; value = 'fairyfly live test (issue 158)' }) 'title'
        Assert-Ok (Invoke-Tool $Srv 'gui_element_click' @{ element = 'wnd[1]/tbar[0]/btn[0]' }) 'Save attributes'
        Assert-Ok (Invoke-Tool $Srv 'gui_element_click' @{ element = 'wnd[2]/tbar[0]/btn[7]' }) 'Local Object'
    }
    $title = [string](Invoke-Com (Invoke-Com (Get-SapSession) 'FindById' 'InvokeMethod' @('wnd[0]')) 'Text' 'GetProperty')
    if ($title -notmatch "Change.*$Program") { throw "not in change mode of $Program (window: $title)" }
}

$srv = $null
try {
    $srv = Start-Server 'mcp --allow-write'
    if ((Get-Text (Invoke-Tool $srv 'gui_session_list')) -notmatch 'ses\[0\]') { Write-Host 'no SAP session'; exit 2 }
    try { Open-ProgramInChangeMode $srv } catch { Write-Host "setup failed: $($_.Exception.Message)"; exit 2 }

    $stamp = Get-Date -Format 'yyyyMMddHHmmss'
    $source = "REPORT zffy_fill_test.`n* fairyfly live check, issue 158, run $stamp`nWRITE 'filled by fairyfly'."

    Check 'abap.fill_change_mode' {
        $r = Invoke-Tool $srv 'gui_element_fill' @{ element = $Editor; value = $source }
        Assert-Ok $r 'fill'
        Assert-That ((Get-Data $r).value -eq $source) "the answer does not show the source: $((Get-Data $r).value)"
        $read = Get-Data (Invoke-Tool $srv 'gui_element_get' @{ element = $Editor })
        Assert-That ($read.value -eq $source) "the editor shows: $($read.value)"
    }
    Check 'abap.long_source' {
        # more lines than one read page (200): every line is compared, the answer shows the first page only
        $long = "REPORT zffy_fill_test." + (-join (1..250 | ForEach-Object { "`nWRITE / 'line $_'." }))
        $r = Invoke-Tool $srv 'gui_element_fill' @{ element = $Editor; value = $long }
        Assert-Ok $r 'long fill'
        $d = Get-Data $r
        Assert-That ($d.source_total_lines -eq 251) "source_total_lines $($d.source_total_lines)"
        Assert-That ($d.source_truncated -eq $true) 'the answer is not marked truncated'
        Assert-Ok (Invoke-Tool $srv 'gui_element_fill' @{ element = $Editor; value = $source }) 'short fill again'
    }
    Check 'abap.saved' {
        Assert-Ok (Invoke-Tool $srv 'gui_key_send' @{ key = '11' }) 'Save'
        $r = Invoke-Tool $srv 'gui_key_send' @{ key = '27' }   # Activate
        if (Test-PopupOpen) { [void](Invoke-Tool $srv 'gui_key_send' @{ key = 'Enter' }) }   # the inactive-objects list
        Open-ProgramInChangeMode $srv
        $read = Get-Data (Invoke-Tool $srv 'gui_element_get' @{ element = $Editor })
        Assert-That ($read.value -eq $source) "the program opened again shows: $($read.value)"
    }
    Check 'abap.display_mode' {
        Assert-Ok (Invoke-Tool $srv 'gui_key_send' @{ key = '25' }) 'Display <-> Change'
        try {
            $r = Invoke-Tool $srv 'gui_element_fill' @{ element = $Editor; value = 'REPORT zffy_fill_test. "must not land' }
            Assert-That ((Get-ErrCode $r) -eq 'ELEMENT_READ_ONLY') "expected ELEMENT_READ_ONLY, got: $(Get-Text $r)"
            $read = Get-Data (Invoke-Tool $srv 'gui_element_get' @{ element = $Editor })
            Assert-That ($read.value -eq $source) "the display-mode editor shows: $($read.value)"
        } finally { [void](Invoke-Tool $srv 'gui_key_send' @{ key = '25' }) }
    }
    Check 'combo.verified' {
        Assert-Ok (Invoke-Tool $srv 'gui_menu_select' @{ path = 'Goto/Attributes' }) 'Goto > Attributes'
        try {
            $r = Invoke-Tool $srv 'gui_element_fill' @{ element = 'wnd[1]/usr/cmbTRDIR-SUBC'; value = 'Executable program' }
            Assert-Ok $r 'combo fill'
            $d = Get-Data $r
            Assert-That ($d.key -eq '1') "key '$($d.key)' (expected 1)"
            Assert-That ($d.display_value -eq 'Executable program') "display_value '$($d.display_value)'"
        } finally { Close-Popups $srv }
    }
    Check 'menu.trailing_dots' {
        try {
            $r = Invoke-Tool $srv 'gui_menu_select' @{ path = 'Utilities/Settings...' }
            Assert-Ok $r 'Utilities/Settings...'
            Assert-That (Test-PopupOpen) 'no settings popup'
        } finally { Close-Popups $srv }
    }
    Check 'tabs.nested_restore' {
        Assert-Ok (Invoke-Tool $srv 'gui_menu_select' @{ path = 'Utilities/Settings' }) 'Utilities/Settings'
        try {
            Assert-Ok (Invoke-Tool $srv 'gui_element_click' @{ element = 'wnd[1]/usr/tabsG_TABSTRIP/tabp0100' }) 'ABAP Editor tab'
            Assert-Ok (Invoke-Tool $srv 'gui_element_click' @{ element = $SettingsEditTab }) 'Editor tab'
            $r = Invoke-Tool $srv 'gui_screen_read' @{ only = 'editable'; skip_trees = $true }
            Assert-Ok $r 'screen read'
            $outer = Get-SelectedTabName 'wnd[1]/usr/tabsG_TABSTRIP'
            Assert-That ($outer -eq '0100') "the outer strip was left on $outer"
            $inner = Get-SelectedTabName 'wnd[1]/usr/tabsG_TABSTRIP/tabp0100/ssubTOOLAREA:SAPLWB_CUSTOMIZING:0100/tabsG_ED_TABSTR'
            Assert-That ($inner -eq 'EDIT') "the inner strip was left on $inner"
            Assert-That ((Get-Text $r) -match 'PCMODE') 'the read does not show the editor settings'
        } finally { Close-Popups $srv }
    }
    Check 'radio.selected' {
        Assert-Ok (Invoke-Tool $srv 'gui_menu_select' @{ path = 'Utilities/Settings' }) 'Utilities/Settings'
        try {
            Assert-Ok (Invoke-Tool $srv 'gui_element_click' @{ element = 'wnd[1]/usr/tabsG_TABSTRIP/tabp0100' }) 'ABAP Editor tab'
            Assert-Ok (Invoke-Tool $srv 'gui_element_click' @{ element = $SettingsEditTab }) 'Editor tab'
            $current = Get-Data (Invoke-Tool $srv 'gui_element_get' @{ element = "$SettingsRadios/radSEUCUSTOM-ABAPCNTRL" })
            $r = Invoke-Tool $srv 'gui_element_fill' @{ element = "$SettingsRadios/rad*SEUCUSTOM-PCMODE"; value = 'true' }
            Assert-Ok $r 'radio fill'
            $d = Get-Data $r
            Assert-That ($d.selected -eq $true) "selected is '$($d.selected)'"
            Assert-That ($d.value -eq 'Text-Based Editor') "value '$($d.value)'"
            if ($current.selected -eq $true) {   # put the original choice back before the popup is cancelled
                [void](Invoke-Tool $srv 'gui_element_fill' @{ element = "$SettingsRadios/radSEUCUSTOM-ABAPCNTRL"; value = 'true' })
            }
        } finally { Close-Popups $srv }
    }
    Check 'menu.not_found_context' {
        $r = Invoke-Tool $srv 'gui_menu_select' @{ path = 'Utilities/No Such Entry' }
        Assert-That ((Get-ErrCode $r) -eq 'MENU_ITEM_NOT_FOUND') "expected MENU_ITEM_NOT_FOUND, got: $(Get-Text $r)"
        $e = Get-ErrorObject $r
        Assert-That ($e.failed_segment -eq 'No Such Entry') "failed_segment '$($e.failed_segment)'"
        Assert-That (@($e.available) -contains 'Settings') "available: $(@($e.available) -join ', ')"
        Assert-That ($e.transaction -eq 'SE38') "transaction '$($e.transaction)'"
        Assert-That ([string]$e.program -ne '') 'no program'
    }
    Check 'find.text_contains' {
        $r = Invoke-Tool $srv 'gui_screen_find' @{ text_contains = 'pattern' }
        Assert-Ok $r 'find'
        $hits = @((Get-Data $r).elements)
        Assert-That ($hits.Count -ge 1) 'nothing found'
        Assert-That (@($hits | Where-Object { ([string]$_.text + ' ' + [string]$_.tooltip) -match '(?i)pattern' }).Count -eq $hits.Count) 'a hit without the text'
    }
} finally {
    if ($srv) { try { Close-Popups $srv } catch { } }
    Stop-Server $srv
}
Write-Host ("{0} passed, {1} failed" -f $script:Pass, $script:Fail)
if ($script:Fail -gt 0) { exit 1 }
exit 0
