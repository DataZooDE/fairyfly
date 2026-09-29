<#
.SYNOPSIS
  Repeatable read-only regression / soak suite for fairyfly against the Bigfox (SAP A4H) system.

.DESCRIPTION
  Requires a running, logged-in SAP GUI session (/app/con[0]/ses[0]) sitting on the SAP Easy Access screen and
  a user with display authorizations. Uses transactions RZ11, ST22, SM37, SU01 (display only), SM59, SE80.
  Only selection-screen search values are ever filled. Buttons that are deliberately NEVER pressed:
  SM37 btn[46] Release, btn[25] Stop, btn[14] Delete; SU01 btn[8] Create, btn[20] Change Password;
  no Save anywhere. The script always ends with `tcode /n`.
  Never reads trial.env and never prints credentials.

  Exit code: 0 all checks passed (SKIP does not fail), 1 at least one FAIL, 2 no SAP session.

.PARAMETER Exe       Path to fairyfly.exe (default: build\Release\fairyfly.exe under the repo root).
.PARAMETER Iterations Repeat the whole suite N times (soak mode when > 1).
.PARAMETER SlowMs    Calls slower than this are listed in the soak report.
.PARAMETER LogFile   Optional file that receives a copy of the console output.
.PARAMETER NoDestructiveGuardTests  Skip the --read-only refusal checks.
.PARAMETER DryRun    Print the planned checks and fairyfly command lines; do not invoke fairyfly.
#>
param(
    [string]$Exe = '',
    [int]$Iterations = 1,
    [int]$SlowMs = 3000,
    [string]$LogFile = '',
    [switch]$NoDestructiveGuardTests,
    [switch]$DryRun
)

$ErrorActionPreference = 'Stop'
$repoRoot = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
if (-not $Exe) {
    $Exe = Join-Path $repoRoot 'build\Release\fairyfly.exe'
    if (-not (Test-Path -LiteralPath $Exe)) {
        $alt = Join-Path $repoRoot 'build\bin\Release\fairyfly.exe'
        if (Test-Path -LiteralPath $alt) { $Exe = $alt }
    }
}
if ($Iterations -lt 1) { $Iterations = 1 }

# ---------------------------------------------------------------- constants
$S = '/app/con[0]/ses[0]'
$U = "$S/wnd[0]/usr"
$script:Conn = ''
$script:Iter = 0
$script:Calls = New-Object System.Collections.ArrayList
$script:Results = New-Object System.Collections.ArrayList   # objects: Iter, Name, Status, Ms, Msg
$script:Broken = @{}
$script:JobName = ''
$script:SuTabBefore = ''
$script:TempFiles = New-Object System.Collections.ArrayList

function Out-Line([string]$Text) {
    Write-Host $Text
    if ($LogFile) { try { Add-Content -LiteralPath $LogFile -Value $Text -Encoding ASCII } catch { } }
}

# ---------------------------------------------------------------- fairyfly call helper
function ConvertTo-ArgString([string[]]$Items) {
    $parts = foreach ($a in $Items) {
        if ($a -eq '' -or $a -match '[\s"]') { '"' + ($a -replace '(\\*)"', '$1$1\"') + '"' } else { $a }
    }
    return ($parts -join ' ')
}

function Get-DisplayCmd([string[]]$Items) {
    # Redact anything that follows a credential-ish flag; fills only carry search values, but stay defensive.
    $out = @(); $redact = $false
    foreach ($a in $Items) {
        if ($redact) { $out += '***'; $redact = $false; continue }
        if ($a -match '^--(password|credentials-file|credentials-stdin)$') { $redact = $true }
        $out += $a
    }
    return ($out -join ' ')
}

function Invoke-FF {
    param([string[]]$FfArgs, [switch]$NoConn, [int]$TimeoutSec = 120)
    $all = @($FfArgs)
    if (-not $NoConn -and $script:Conn -ne '') { $all += @('--connection', $script:Conn) }
    $shown = Get-DisplayCmd $all
    if ($DryRun) {
        Out-Line ("      > fairyfly " + $shown)
        return [pscustomobject]@{ Ms = 0; ExitCode = 0; Raw = ''; Json = $null; Ok = $true; Code = ''; Msg = '' }
    }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = ConvertTo-ArgString $all
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
    $psi.StandardErrorEncoding = [System.Text.Encoding]::UTF8
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $p = New-Object System.Diagnostics.Process
    $p.StartInfo = $psi
    [void]$p.Start()
    $outTask = $p.StandardOutput.ReadToEndAsync()
    $errTask = $p.StandardError.ReadToEndAsync()
    $timedOut = $false
    if (-not $p.WaitForExit($TimeoutSec * 1000)) {
        $timedOut = $true
        try { $p.Kill() } catch { }
    }
    $p.WaitForExit()
    $sw.Stop()
    $raw = $outTask.Result
    $null = $errTask.Result
    $exit = if ($timedOut) { -1 } else { $p.ExitCode }
    $p.Dispose()
    $json = $null
    try { if ($raw -and $raw.TrimStart().StartsWith('{')) { $json = $raw | ConvertFrom-Json } } catch { $json = $null }
    $ok = $false; $code = ''; $msg = ''
    if ($timedOut) { $code = 'TIMEOUT'; $msg = "no result after $TimeoutSec s" }
    elseif ($null -ne $json) {
        $ok = ($json.status -eq 'success')
        if ($json.error) { $code = [string]$json.error.code; $msg = [string]$json.error.message }
    }
    [void]$script:Calls.Add([pscustomobject]@{ Iter = $script:Iter; Ms = [int]$sw.ElapsedMilliseconds; Cmd = $shown })
    return [pscustomobject]@{ Ms = [int]$sw.ElapsedMilliseconds; ExitCode = $exit; Raw = $raw; Json = $json; Ok = $ok; Code = $code; Msg = $msg }
}

function Assert-That($Cond, [string]$Message) {
    if ($DryRun) { return }
    if (-not $Cond) { throw $Message }
}

function Assert-Ok($R, [string]$What) {
    if ($DryRun) { return }
    if (-not $R.Ok) { throw ("$What failed: code=" + $R.Code + " msg=" + $R.Msg) }
}

function Skip([string]$Why) { throw ("SKIP: " + $Why) }

function Require-Chain([string[]]$Keys) {
    if ($DryRun) { return }
    foreach ($k in $Keys) { if ($script:Broken.ContainsKey($k)) { Skip ("prerequisite '$k' did not pass") } }
}

# ---------------------------------------------------------------- JSON helpers
function Get-Ids($Json) {
    $ids = New-Object System.Collections.ArrayList
    if ($null -eq $Json -or $null -eq $Json.data) { return @() }
    foreach ($e in @($Json.data.elements)) { if ($e -and $e.id) { [void]$ids.Add([string]$e.id) } }
    foreach ($t in @($Json.data.tabs)) {
        if ($t -and $t.elements) { foreach ($e in @($t.elements)) { if ($e -and $e.id) { [void]$ids.Add([string]$e.id) } } }
    }
    return @($ids | Sort-Object -Unique)
}

function Find-Tables($Node, $Acc) {
    if ($null -eq $Node) { return }
    if ($Node -is [string]) { return }
    if ($Node -is [System.Collections.IEnumerable]) { foreach ($i in $Node) { Find-Tables $i $Acc }; return }
    if ($Node -is [pscustomobject]) {
        if ($Node.PSObject.Properties['table_data'] -and $Node.table_data) { [void]$Acc.Add($Node) }
        foreach ($p in $Node.PSObject.Properties) { Find-Tables $p.Value $Acc }
    }
}

function Get-Tables($Json) {
    $acc = New-Object System.Collections.ArrayList
    if ($null -ne $Json) { Find-Tables $Json.data $acc }
    return $acc
}

function Get-RowCount($Table) {
    if ($null -eq $Table -or $null -eq $Table.table_data -or $null -eq $Table.table_data.rows) { return 0 }
    return @($Table.table_data.rows).Count
}

function Find-Prop($Node, [string]$Name) {
    if ($null -eq $Node -or $Node -is [string]) { return $null }
    if ($Node -is [System.Collections.IEnumerable]) {
        foreach ($i in $Node) { $v = Find-Prop $i $Name; if ($null -ne $v) { return $v } }
        return $null
    }
    if ($Node -is [pscustomobject]) {
        if ($Node.PSObject.Properties[$Name]) { return $Node.$Name }
        foreach ($p in $Node.PSObject.Properties) { $v = Find-Prop $p.Value $Name; if ($null -ne $v) { return $v } }
    }
    return $null
}

function Get-BatchResults([string]$Raw) {
    $list = @()
    foreach ($line in ($Raw -split "`r?`n")) {
        if ($line.Trim().StartsWith('{')) { try { $list += ,($line | ConvertFrom-Json) } catch { $list += ,$null } }
    }
    return $list
}

function New-TempFile([string]$Prefix, [string[]]$Lines) {
    $path = Join-Path ([System.IO.Path]::GetTempPath()) ($Prefix + [guid]::NewGuid().ToString('N') + '.txt')
    if ($DryRun) {
        Out-Line "      (batch file $Prefix*.txt would contain:)"
        foreach ($l in $Lines) { Out-Line "        $l" }
        return $path
    }
    [System.IO.File]::WriteAllLines($path, $Lines, (New-Object System.Text.UTF8Encoding($false)))
    [void]$script:TempFiles.Add($path)
    return $path
}

# ---------------------------------------------------------------- navigation helpers
function Go([string]$Tcode) {
    $r = Invoke-FF @('tcode', $Tcode)
    Assert-Ok $r "tcode $Tcode"
    return $r
}

function Reset-Session {
    if ($DryRun) { return }
    try { [void](Invoke-FF @('close')) } catch { }
    try { [void](Invoke-FF @('tcode', '/n')) } catch { }
}

function Run-Check {
    param([string]$Name, [scriptblock]$Body, [string]$Chain = '')
    if ($DryRun) {
        Out-Line "PLAN  $Name"
        try { & $Body } catch { }
        return
    }
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $status = 'PASS'; $msg = ''
    try { & $Body }
    catch {
        $m = $_.Exception.Message
        if ($m -like 'SKIP:*') { $status = 'SKIP'; $msg = $m.Substring(5).Trim() }
        else { $status = 'FAIL'; $msg = $m }
    }
    $sw.Stop()
    if ($status -ne 'PASS' -and $Chain) { $script:Broken[$Chain] = $true }
    if ($status -eq 'FAIL') { Reset-Session }
    $line = "$status $Name [$([int]$sw.ElapsedMilliseconds)ms]"
    if ($msg) { $line += " - $msg" }
    Out-Line $line
    [void]$script:Results.Add([pscustomobject]@{ Iter = $script:Iter; Name = $Name; Status = $status; Ms = [int]$sw.ElapsedMilliseconds; Msg = $msg })
}

# ---------------------------------------------------------------- the suite
function Invoke-Suite {
    $script:Broken = @{}
    $script:JobName = ''
    $script:SuTabBefore = ''
    $grid = "$U/cntlRSSHOWRABAX_ALV_100/shellcont/shell"
    $chkFinished = "$U/chkBTCH2170-FINISHED"

    # -- attach / connections
    Run-Check 'attach.dedupe' {
        $r = Invoke-FF @('attach', '--session-id', $S) -NoConn
        Assert-Ok $r 'attach'
        if (-not $DryRun) { $script:Conn = [string]$r.Json.data.connection_file_id }
        Assert-That ($script:Conn -ne '') 'attach returned no connection_file_id'
        $c = Invoke-FF @('connections') -NoConn
        Assert-Ok $c 'connections'
        $n = @(@($c.Json.data.connections) | Where-Object { $_.session_id -eq $S }).Count
        Assert-That ($n -eq 1) "connections lists $n entries for $S (expected exactly 1)"
    } -Chain 'attach'

    # -- tcode
    Run-Check 'tcode.SM37_and_back' {
        Require-Chain @('attach')
        $r = Go '/nSM37'
        Assert-That ($r.Json.data.actual_tcode -eq 'SM37') ("actual_tcode=" + $r.Json.data.actual_tcode)
        [void](Go '/n')
    }

    # -- RZ11
    Run-Check 'RZ11.unknown_and_known_param' {
        Require-Chain @('attach')
        [void](Go '/nRZ11')
        Assert-Ok (Invoke-FF @('fill', "$U/ctxtTPFYSTRUCT-NAME", 'rdisp/max_wprun_time')) 'fill param'
        $r = Invoke-FF @('click', "$U/btnPANZEIGEN_1000")
        $txt = if ($r.Ok) { [string]$r.Json.data.status_bar.text } else { [string]$r.Json.error.status_bar.text }
        Assert-That ($txt -match 'not known') "status bar was '$txt' (expected 'not known')"
        Assert-Ok (Invoke-FF @('fill', "$U/ctxtTPFYSTRUCT-NAME", 'rdisp/wp_no_dia')) 'fill param 2'
        Assert-Ok (Invoke-FF @('click', "$U/btnPANZEIGEN_1000")) 'click display'
        $m = Invoke-FF @('screen', 'read', '--no-tabs', '--output', 'markdown')
        Assert-That ($m.Raw -match 'Instance Profile') "markdown lacks 'Instance Profile'"
        Assert-That ($m.Raw -match '(?s)Instance Profile.{0,600}?\d+') 'no numeric result after Instance Profile'
        [void](Go '/n')
    }

    # -- ST22 list + dump
    Run-Check 'ST22.list_today' {
        Require-Chain @('attach')
        [void](Go '/nST22')
        Assert-Ok (Invoke-FF @('click', "$U/btnTODAY")) 'click btnTODAY'
        $r = Invoke-FF @('screen', 'read', '--no-tabs', '--max-rows', '5', '--output', 'json')
        Assert-Ok $r 'screen read'
        $g = @(Get-Tables $r.Json) | Where-Object { $_.id -eq $grid } | Select-Object -First 1
        if (-not $DryRun -and $null -eq $g) { Skip 'ST22 list grid not present (no dumps today)' }
        if (-not $DryRun -and (Get-RowCount $g) -lt 1) { Skip 'no dumps today' }
    } -Chain 'ST22L'

    Run-Check 'ST22.open_dump_doubleclick' {
        Require-Chain @('ST22L')
        Assert-Ok (Invoke-FF @('click', $grid, '--row', '0', '--column', 'GPROGRAM', '--doubleclick')) 'doubleclick'
        $m = Invoke-FF @('screen', 'read', '--no-tabs', '--output', 'markdown')
        Assert-That ($m.Raw -match 'Error analysis|Short Text') "dump screen lacks 'Error analysis'/'Short Text'"
    } -Chain 'ST22D'

    Run-Check 'ST22.menu_enumerate' {
        Require-Chain @('ST22D')
        $r = Invoke-FF @('screen', 'menu')     # enumerate only, never --select
        Assert-Ok $r 'screen menu'
    }

    Run-Check 'ST22.send_key_f3_back' {
        Require-Chain @('ST22D')
        Assert-Ok (Invoke-FF @('send-key', 'f3')) 'send-key f3'
    }

    Run-Check 'ST22.send_key_invalid' {
        Require-Chain @('attach')
        $r = Invoke-FF @('send-key', 'bogus')
        Assert-That ((-not $r.Ok) -and $r.Code -eq 'INVALID_VKEY') "expected INVALID_VKEY, got '$($r.Code)'"
    }

    Run-Check 'ST22.detail_popup_close' {
        Require-Chain @('ST22L')
        [void](Go '/nST22')
        Assert-Ok (Invoke-FF @('click', "$U/btnTODAY")) 'click btnTODAY'
        Assert-Ok (Invoke-FF @('click', $grid, '--row', '0', '--column', 'ERRORID')) 'select row 0'
        Assert-Ok (Invoke-FF @('click', "$grid/btn_&DETAIL")) 'click Details'
        $m = Invoke-FF @('screen', 'read', '--no-tabs', '--output', 'markdown')
        Assert-That ($m.Raw -match 'Details') "popup 'Details' not visible"
        $c1 = Invoke-FF @('close')
        Assert-Ok $c1 'close'
        Assert-That (@('vkey', 'window_close') -contains [string]$c1.Json.data.method) "close method '$($c1.Json.data.method)'"
        $c2 = Invoke-FF @('close')
        Assert-That ((-not $c2.Ok) -and $c2.Code -eq 'NO_POPUP') "second close: expected NO_POPUP, got '$($c2.Code)'"
    }

    # -- ST22 selection screen
    Run-Check 'ST22.selection_caption_and_carriers' {
        Require-Chain @('attach')
        [void](Go '/nST22')
        $m = Invoke-FF @('screen', 'read', '--no-tabs', '--output', 'markdown')
        Assert-That ($m.Raw -match 'Date \(to\)') "markdown lacks 'Date (to)' range caption"
        $j = Invoke-FF @('screen', 'read', '--no-tabs', '--output', 'json')
        # Only the label carriers (-TEXT / -TO_TEXT) are collapsed; the -VALU_PUSH buttons (multiple selection) are real controls.
Assert-That ($j.Raw -notmatch '_%_APP_%-(TO_)?TEXT') 'JSON still contains %_..._%_APP_%-TEXT / -TO_TEXT label carrier elements'
        [void](Go '/n')
    }

    # -- SM37
    Run-Check 'SM37.checkbox_get_and_find' {
        Require-Chain @('attach')
        [void](Go '/nSM37')
        $g = Invoke-FF @('get', $chkFinished)
        Assert-Ok $g 'get FINISHED'
        Assert-That ($g.Json.data.selected -is [bool]) 'data.selected is not boolean'
        Assert-That ([string]$g.Json.data.label -ne '') 'data.label is empty'
        $f = Invoke-FF @('screen', 'find', '--id-contains', 'chkBTCH2170-FINISHED')
        Assert-Ok $f 'screen find'
        $el = @($f.Json.data.elements) | Select-Object -First 1
        Assert-That ($null -ne $el -and ($el.PSObject.Properties.Name -contains 'selected')) 'find result has no selected property'
    } -Chain 'SM37S'

    Run-Check 'SM37.execute_and_finished_row' {
        Require-Chain @('SM37S')
        Assert-Ok (Invoke-FF @('fill', "$U/txtBTCH2170-USERNAME", '*')) 'fill user'
        Assert-Ok (Invoke-FF @('fill', "$U/ctxtBTCH2170-FROM_DATE", '01.01.2026')) 'fill from date'
        Assert-Ok (Invoke-FF @('click', "$S/wnd[0]/tbar[1]/btn[8]")) 'execute'
        $m = Invoke-FF @('screen', 'read', '--no-tabs', '--max-rows', '200', '--output', 'markdown')
        Assert-That ($m.Raw -match 'Finished') "no 'Finished' row in job list"
        # Heuristic: first table row containing 'Finished'; job name = first cell that looks like a name.
        $name = ''
        foreach ($line in ($m.Raw -split "`r?`n")) {
            if ($line -match 'Finished' -and $line -match '\|') {
                foreach ($cell in ($line -split '\|')) {
                    $c = $cell.Trim()
                    if ($c -match '^[A-Za-z_/][A-Za-z0-9_/\.\-]{2,}$' -and $c -ne 'Finished' -and $c -notmatch '^(Released|Canceled|Active|Scheduled|Ready)$') { $name = $c; break }
                }
                if ($name) { break }
            }
        }
        if (-not $DryRun -and -not $name) { throw 'could not derive a job name from the Finished row' }
        $script:JobName = if ($name) { $name } else { '<first-job-name>' }
    } -Chain 'SM37X'

    Run-Check 'SM37.text_contains_job' {
        Require-Chain @('SM37X')
        $r = Invoke-FF @('screen', 'read', '--no-tabs', '--text-contains', $script:JobName, '--output', 'json')
        Assert-Ok $r 'screen read --text-contains'
        Assert-That ($r.Raw.IndexOf($script:JobName, [StringComparison]::OrdinalIgnoreCase) -ge 0) "job '$($script:JobName)' not returned"
    }

    Run-Check 'SM37.joblog_fast' {
        Require-Chain @('SM37X')
        # btn[47] = Job log ONLY. NEVER btn[46] Release, btn[25] Stop, btn[14] Delete.
        $r = Invoke-FF @('click', "$S/wnd[0]/tbar[1]/btn[47]", '--wait-for-window')
        Assert-Ok $r 'click Job log'
        Assert-That ($r.Json.data.screen_changed -eq $true) 'screen_changed is not true'
        Assert-That ($r.Ms -lt 1500) "Job log took $($r.Ms) ms (limit 1500)"
        Assert-Ok (Invoke-FF @('send-key', 'f3')) 'send-key f3'
    } -Chain 'SM37J'

    Run-Check 'SM37.joblog_and_f3_under_read_only' {
        Require-Chain @('SM37J')
        $r = Invoke-FF @('--read-only', 'click', "$S/wnd[0]/tbar[1]/btn[47]", '--wait-for-window')
        Assert-Ok $r 'read-only Job log'
        $b = Invoke-FF @('--read-only', 'send-key', 'f3')
        Assert-Ok $b 'read-only F3'
    }

    # -- SU01 (display only)
    Run-Check 'SU01.display_developer' {
        Require-Chain @('attach')
        [void](Go '/nSU01')
        Assert-Ok (Invoke-FF @('fill', "$U/ctxtSUID_ST_BNAME-BNAME", 'DEVELOPER')) 'fill user name'
        Assert-Ok (Invoke-FF @('click', "$S/wnd[0]/tbar[1]/btn[7]")) 'click Display (F7)'   # NEVER btn[8] Create / btn[20] Change Password
        $t = Invoke-FF @('get', "$U/tabsTABSTRIP1")
        $script:SuTabBefore = if ($t.Ok) { [string]$t.Raw } else { '' }
    } -Chain 'SU'

    Run-Check 'SU01.tabs_find' {
        Require-Chain @('SU')
        $r = Invoke-FF @('screen', 'find', '--type', 'GuiTab', '--limit', '30')
        Assert-Ok $r 'screen find'
        $tabs = @($r.Json.data.elements)
        Assert-That ($tabs.Count -ge 10) "only $($tabs.Count) tabs (expected >= 10)"
        $empty = @($tabs | Where-Object { -not [string]$_.text }).Count
        Assert-That ($empty -eq 0) "$empty tab(s) have empty text"
        Assert-That (@($tabs | Where-Object { $_.text -match 'Roles' }).Count -ge 1) "no 'Roles' tab"
    }

    Run-Check 'SU01.tab_read_actg_fast' {
        Require-Chain @('SU')
        $r = Invoke-FF @('screen', 'read', '--tab', 'tabpACTG', '--max-rows', '50', '--output', 'json')
        Assert-Ok $r 'screen read --tab tabpACTG'
        $withRows = @(Get-Tables $r.Json | Where-Object { (Get-RowCount $_) -ge 1 })
        Assert-That ($withRows.Count -ge 1) 'no table_data grid with >= 1 row in the tab'
        Assert-That ($r.Ms -lt 2500) "tab read took $($r.Ms) ms (limit 2500)"
    }

    Run-Check 'SU01.tab_not_found' {
        Require-Chain @('SU')
        $r = Invoke-FF @('screen', 'read', '--tab', 'tabpNOSUCH', '--output', 'json')
        Assert-That ((-not $r.Ok) -and $r.Code -eq 'TAB_NOT_FOUND') "expected TAB_NOT_FOUND, got '$($r.Code)'"
    }

    Run-Check 'SU01.compact_json' {
        Require-Chain @('SU')
        $full = Invoke-FF @('screen', 'read', '--no-tabs', '--output', 'json')
        $comp = Invoke-FF @('screen', 'read', '--no-tabs', '--compact', '--output', 'json')
        Assert-Ok $full 'full read'; Assert-Ok $comp 'compact read'
        $fmt = Find-Prop $comp.Json 'hierarchy_format'
        Assert-That ($fmt -eq 'ids') "hierarchy_format='$fmt' (expected 'ids')"
        Assert-That ($comp.Raw.Length -lt $full.Raw.Length) "compact ($($comp.Raw.Length)) not smaller than full ($($full.Raw.Length))"
    }

    Run-Check 'SU01.active_tab_unchanged' {
        Require-Chain @('SU')
        if (-not $DryRun -and -not $script:SuTabBefore) { Skip 'tabstrip get unavailable' }
        if (-not $DryRun -and $script:SuTabBefore -notmatch '(?i)select') { Skip 'get on tabsTABSTRIP1 does not expose the selected tab (note only)' }
        $t = Invoke-FF @('get', "$U/tabsTABSTRIP1")
        Assert-Ok $t 'get tabstrip'
        Assert-That ($t.Raw -eq $script:SuTabBefore) 'tabstrip state differs from before the --tab reads'
    }

    # -- element-id stability, gated vs exhaustive
    function Test-IdStability([string]$Label) {
        $a = Invoke-FF @('screen', 'read', '--no-tabs', '--output', 'json') -TimeoutSec 180
        $b = Invoke-FF @('screen', 'read', '--no-tabs', '--probe-all', '--output', 'json') -TimeoutSec 180
        Assert-Ok $a "$Label gated read"; Assert-Ok $b "$Label probe-all read"
        $ia = Get-Ids $a.Json; $ib = Get-Ids $b.Json
        if ($DryRun) { return }
        $onlyA = @($ia | Where-Object { $ib -notcontains $_ })
        $onlyB = @($ib | Where-Object { $ia -notcontains $_ })
        if ($onlyA.Count -or $onlyB.Count) {
            $d = @(); if ($onlyA.Count) { $d += 'gated-only: ' + (($onlyA | Select-Object -First 8) -join ', ') }
            if ($onlyB.Count) { $d += 'probe-all-only: ' + (($onlyB | Select-Object -First 8) -join ', ') }
            throw ("ids differ (" + $ia.Count + " vs " + $ib.Count + "); " + ($d -join ' | '))
        }
    }

    Run-Check 'ids.stable.SU01_display' {
        Require-Chain @('attach')
        [void](Go '/nSU01')
        Assert-Ok (Invoke-FF @('fill', "$U/ctxtSUID_ST_BNAME-BNAME", 'DEVELOPER')) 'fill user name'
        Assert-Ok (Invoke-FF @('click', "$S/wnd[0]/tbar[1]/btn[7]")) 'click Display (F7)'
        Test-IdStability 'SU01 display'
    }
    Run-Check 'ids.stable.ST22_selection' {
        Require-Chain @('attach')
        [void](Go '/nST22')
        Test-IdStability 'ST22 selection'
    }
    Run-Check 'ids.stable.SM37_selection' {
        Require-Chain @('attach')
        [void](Go '/nSM37')
        Test-IdStability 'SM37 selection'
    }

    # -- unknown-shell cache regression via batch
    Run-Check 'batch.unknown_shell_cache_ST22' {
        Require-Chain @('attach')
        $c = $script:Conn
        $lines = @(
            "tcode /nSM59 --connection $c",
            "screen read --no-tabs --output json --connection $c",
            "tcode /nSE80 --connection $c",
            "screen read --no-tabs --output json --connection $c",
            "tcode /nST22 --connection $c",
            "click $U/btnTODAY --connection $c",
            "screen read --no-tabs --max-rows 50 --output json --connection $c"
        )
        $bf = New-TempFile 'ff_shellcache_' $lines
        $r = Invoke-FF @('batch', '--file', $bf) -NoConn -TimeoutSec 300
        $res = Get-BatchResults $r.Raw
        if (-not $DryRun) {
            Assert-That ($res.Count -eq 7) "batch returned $($res.Count) results (expected 7)"
            Assert-That ($res[6] -and $res[6].status -eq 'success') 'final batch read failed'
        }
        $fresh = Invoke-FF @('screen', 'read', '--no-tabs', '--max-rows', '50', '--output', 'json')
        Assert-Ok $fresh 'fresh read'
        if (-not $DryRun) {
            $tb = Get-Tables $res[6]; $tf = Get-Tables $fresh.Json
            $gb = @($tb | Where-Object { $_.id -eq $grid } | Select-Object -First 1)[0]
            $gf = @($tf | Where-Object { $_.id -eq $grid } | Select-Object -First 1)[0]
            if ($null -eq $gf) { Skip 'ST22 grid absent in fresh read (no dumps today)' }
            Assert-That ($null -ne $gb) 'ST22 grid missing after SM59/SE80 in batch (unknown-shell cache regression)'
            $nb = Get-RowCount $gb; $nf = Get-RowCount $gf
            Assert-That ($nb -eq $nf) "row count batch=$nb fresh=$nf"
        }
        [void](Go '/n')
    }

    # -- output / read-only guard
    Run-Check 'list.toon_starts_with_data' {
        $r = Invoke-FF @('--output', 'toon', 'list') -NoConn
        Assert-That ($r.Raw.TrimStart().StartsWith('data:')) 'toon output does not start with data:'
    }

    if (-not $NoDestructiveGuardTests) {
        Run-Check 'read_only.refusals' {
            Require-Chain @('attach')
            [void](Go '/nSU01')     # Save button (tbar[0]/btn[11]) exists on every screen; refusals change nothing
            $r1 = Invoke-FF @('--read-only', 'send-key', 'f11')
            Assert-That ((-not $r1.Ok) -and $r1.Code -eq 'READ_ONLY_REFUSED') "send-key f11: '$($r1.Code)'"
            $r2 = Invoke-FF @('--read-only', 'click', "$S/wnd[0]/tbar[0]/btn[11]")
            Assert-That ((-not $r2.Ok) -and $r2.Code -eq 'READ_ONLY_REFUSED') "click btn[11]: '$($r2.Code)'"
            $r3 = Invoke-FF @('--read-only', 'screen', 'menu', '--select', 'System/Delete')
            Assert-That ((-not $r3.Ok) -and $r3.Code -eq 'READ_ONLY_REFUSED') "menu System/Delete: '$($r3.Code)'"
            $r4 = Invoke-FF @('--read-only', 'fill', "$U/ctxtSUID_ST_BNAME-BNAME", 'DEVELOPER')
            Assert-That ((-not $r4.Ok) -and $r4.Code -eq 'READ_ONLY_REFUSED') "fill: '$($r4.Code)'"
            [void](Go '/n')
        }
    }

    # -- batch --file with 8 read-only commands
    Run-Check 'batch.eight_readonly_commands' {
        Require-Chain @('attach')
        $c = $script:Conn
        $lines = @(
            "tcode /nSM37 --connection $c",
            "get $chkFinished --connection $c",
            "screen find --id-contains chkBTCH2170-FINISHED --connection $c",
            "screen read --no-tabs --max-rows 20 --connection $c",
            "get $U/txtBTCH2170-USERNAME --connection $c",
            "screen menu --connection $c",
            "tcode /n --connection $c",
            "list"
        )
        $bf = New-TempFile 'ff_batch8_' $lines
        $r = Invoke-FF @('batch', '--file', $bf) -NoConn -TimeoutSec 300
        if (-not $DryRun) {
            $res = Get-BatchResults $r.Raw
            Assert-That ($res.Count -eq 8) "batch returned $($res.Count) results (expected 8)"
            $bad = @(); for ($i = 0; $i -lt $res.Count; $i++) { if (-not $res[$i] -or $res[$i].status -ne 'success') { $bad += ($i + 1) } }
            Assert-That ($bad.Count -eq 0) ("failing batch lines: " + ($bad -join ','))
            Out-Line "      batch of 8: total $($r.Ms) ms"
        }
    }

    # -- final: no modal popup left behind
    Run-Check 'final.no_popup_left' {
        $c = Invoke-FF @('close')
        Assert-That ((-not $c.Ok) -and $c.Code -eq 'NO_POPUP') "a popup was still open (close returned '$($c.Code)'; it has now been closed)"
    }
}

# ---------------------------------------------------------------- main
Out-Line ("fairyfly Bigfox regression - exe: $Exe - iterations: $Iterations" + $(if ($DryRun) { ' - DRY RUN' } else { '' }))
if (-not $DryRun) {
    if (-not (Test-Path -LiteralPath $Exe)) { Out-Line "ERROR: fairyfly executable not found: $Exe"; exit 2 }
    $pre = Invoke-FF @('attach', '--session-id', $S) -NoConn
    if (-not $pre.Ok) { Out-Line "ERROR: no SAP GUI session ($S): code=$($pre.Code) msg=$($pre.Msg)"; exit 2 }
    $script:Conn = [string]$pre.Json.data.connection_file_id
} else {
    $script:Conn = '<conn-id>'
}

$overall = [System.Diagnostics.Stopwatch]::StartNew()
try {
    for ($i = 1; $i -le $Iterations; $i++) {
        $script:Iter = $i
        if ($Iterations -gt 1 -or $DryRun) { Out-Line "=== iteration $i/$Iterations ===" }
        try { Invoke-Suite }
        finally { Reset-Session }
        if ($DryRun) { break }
    }
}
finally {
    foreach ($f in $script:TempFiles) { try { Remove-Item -LiteralPath $f -Force -ErrorAction SilentlyContinue } catch { } }
    if (-not $DryRun) { try { [void](Invoke-FF @('tcode', '/n')) } catch { } }
}
$overall.Stop()

if ($DryRun) { Out-Line 'Dry run complete: fairyfly was not invoked.'; exit 0 }

# ---------------------------------------------------------------- summary
$pass = @($script:Results | Where-Object { $_.Status -eq 'PASS' }).Count
$fail = @($script:Results | Where-Object { $_.Status -eq 'FAIL' }).Count
$skip = @($script:Results | Where-Object { $_.Status -eq 'SKIP' }).Count
Out-Line ''
Out-Line ("SUMMARY: $pass passed, $fail failed, $skip skipped over $Iterations iteration(s); $($script:Calls.Count) fairyfly calls; total $([int]($overall.Elapsed.TotalSeconds)) s")

if ($Iterations -gt 1) {
    $intermittent = @($script:Results | Group-Object Name | Where-Object {
        $st = @($_.Group | ForEach-Object { $_.Status })
        ($st -contains 'PASS') -and ($st -contains 'FAIL')
    })
    if ($intermittent.Count) {
        Out-Line 'INTERMITTENT checks (passed in some iterations, failed in others):'
        foreach ($g in $intermittent) {
            $f = @($g.Group | Where-Object { $_.Status -eq 'FAIL' })
            Out-Line ("  " + $g.Name + ": failed in iteration(s) " + (($f | ForEach-Object { $_.Iter }) -join ',') + " - " + $f[0].Msg)
        }
    } else { Out-Line 'No intermittent checks.' }
}

Out-Line 'Slowest 10 calls:'
foreach ($c in ($script:Calls | Sort-Object Ms -Descending | Select-Object -First 10)) {
    Out-Line ("  {0,6} ms  iter {1}  {2}" -f $c.Ms, $c.Iter, $c.Cmd)
}
$slow = @($script:Calls | Where-Object { $_.Ms -gt $SlowMs } | Sort-Object Ms -Descending)
Out-Line "Calls slower than $SlowMs ms: $($slow.Count)"
foreach ($c in $slow) { Out-Line ("  {0,6} ms  iter {1}  {2}" -f $c.Ms, $c.Iter, $c.Cmd) }

if ($fail -gt 0) {
    Out-Line 'FAILED checks:'
    foreach ($r in ($script:Results | Where-Object { $_.Status -eq 'FAIL' })) { Out-Line ("  iter {0} {1}: {2}" -f $r.Iter, $r.Name, $r.Msg) }
    exit 1
}
exit 0
