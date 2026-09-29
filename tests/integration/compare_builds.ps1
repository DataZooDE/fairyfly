<#
.SYNOPSIS
  Compare two fairyfly builds on a fixed list of read-only Bigfox (SAP A4H) screens.

.DESCRIPTION
  For each screen: navigate ONCE with NewExe, then run `screen read --no-tabs --max-rows 100 --output json`
  with both builds (best of 2 runs each), compare the sorted element-id sets and print
  Screen / OldMs / NewMs / OldEl / NewEl / OnlyOld / OnlyNew / OldKB / NewKB.
  Prints the first 8 differing ids per screen. Exit code 1 if any id differs, 2 if no SAP session, else 0.

  Prerequisites: logged-in Bigfox session /app/con[0]/ses[0] on SAP Easy Access, display authorizations.
  Only selection-screen search values are filled (TADIR, DEVELOPER, rdisp/wp_no_dia, '*', a date).
  Never presses Save/Delete/Release/Stop/Create/Change. Ends with `tcode /n`.

.PARAMETER OldExe  Baseline fairyfly.exe.
.PARAMETER NewExe  Candidate fairyfly.exe (also used for navigation).
.PARAMETER Screens Optional subset of screen names (see the list below; matching is case-insensitive).
.PARAMETER DryRun  Print the navigation plan only.
#>
param(
    [Parameter(Mandatory = $true)][string]$OldExe,
    [Parameter(Mandatory = $true)][string]$NewExe,
    [string[]]$Screens = @(),
    [switch]$DryRun
)
$ErrorActionPreference = 'Stop'
$S = '/app/con[0]/ses[0]'
$U = "$S/wnd[0]/usr"
$script:Conn = ''

function ConvertTo-ArgString([string[]]$Items) {
    $parts = foreach ($a in $Items) {
        if ($a -eq '' -or $a -match '[\s"]') { '"' + ($a -replace '(\\*)"', '$1$1\"') + '"' } else { $a }
    }
    return ($parts -join ' ')
}

function Invoke-Build {
    param([string]$Exe, [string[]]$FfArgs, [int]$TimeoutSec = 180)
    $all = @($FfArgs)
    if ($script:Conn -ne '') { $all += @('--connection', $script:Conn) }
    if ($DryRun) { Write-Host ("      > " + (Split-Path -Leaf $Exe) + " " + ($all -join ' ')); return [pscustomobject]@{ Ms = 0; Raw = ''; Json = $null; Ok = $true; Code = '' } }
    $psi = New-Object System.Diagnostics.ProcessStartInfo
    $psi.FileName = $Exe
    $psi.Arguments = ConvertTo-ArgString $all
    $psi.UseShellExecute = $false
    $psi.RedirectStandardOutput = $true
    $psi.RedirectStandardError = $true
    $psi.CreateNoWindow = $true
    $psi.StandardOutputEncoding = [System.Text.Encoding]::UTF8
    $sw = [System.Diagnostics.Stopwatch]::StartNew()
    $p = New-Object System.Diagnostics.Process
    $p.StartInfo = $psi
    [void]$p.Start()
    $o = $p.StandardOutput.ReadToEndAsync()
    $e = $p.StandardError.ReadToEndAsync()
    $timedOut = -not $p.WaitForExit($TimeoutSec * 1000)
    if ($timedOut) { try { $p.Kill() } catch { } }
    $p.WaitForExit(); $sw.Stop()
    $raw = $o.Result; $null = $e.Result; $p.Dispose()
    $json = $null
    try { if ($raw -and $raw.TrimStart().StartsWith('{')) { $json = $raw | ConvertFrom-Json } } catch { }
    $code = ''
    if ($timedOut) { $code = 'TIMEOUT' } elseif ($json -and $json.error) { $code = [string]$json.error.code }
    return [pscustomobject]@{ Ms = [int]$sw.ElapsedMilliseconds; Raw = $raw; Json = $json; Ok = ($json -and $json.status -eq 'success'); Code = $code }
}

function Get-Ids($Json) {
    $ids = New-Object System.Collections.ArrayList
    if ($null -eq $Json -or $null -eq $Json.data) { return @() }
    foreach ($e in @($Json.data.elements)) { if ($e -and $e.id) { [void]$ids.Add([string]$e.id) } }
    foreach ($t in @($Json.data.tabs)) {
        if ($t -and $t.elements) { foreach ($e in @($t.elements)) { if ($e -and $e.id) { [void]$ids.Add([string]$e.id) } } }
    }
    return @($ids | Sort-Object -Unique)
}

# Navigation steps are argument arrays run with NewExe. Only selection-screen values are filled.
$catalog = [ordered]@{
    'EasyAccess'      = @(, @('tcode', '/n'))
    'ST22-selection'  = @(, @('tcode', '/nST22'))
    'ST22-list'       = @(@('tcode', '/nST22'), @('click', "$U/btnTODAY"))
    'SM37-selection'  = @(, @('tcode', '/nSM37'))
    'SM37-joblist'    = @(@('tcode', '/nSM37'), @('fill', "$U/txtBTCH2170-USERNAME", '*'), @('fill', "$U/ctxtBTCH2170-FROM_DATE", '01.01.2026'), @('click', "$S/wnd[0]/tbar[1]/btn[8]"))
    'RZ11-detail'     = @(@('tcode', '/nRZ11'), @('fill', "$U/ctxtTPFYSTRUCT-NAME", 'rdisp/wp_no_dia'), @('click', "$U/btnPANZEIGEN_1000"))
    'SU01-selection'  = @(, @('tcode', '/nSU01'))
    'SU01-display'    = @(@('tcode', '/nSU01'), @('fill', "$U/ctxtSUID_ST_BNAME-BNAME", 'DEVELOPER'), @('click', "$S/wnd[0]/tbar[1]/btn[7]"))
    'SM50'            = @(, @('tcode', '/nSM50'))
    'RZ04'            = @(, @('tcode', '/nRZ04'))
    'SE16-TADIR'      = @(@('tcode', '/nSE16'), @('fill', "$U/ctxtDATABROWSE-TABLENAME", 'TADIR'), @('send-key', 'f7'))
    'SE11-TADIR'      = @(@('tcode', '/nSE11'), @('fill', "$U/ctxtRSRD1-TBMA_VAL", 'TADIR'), @('send-key', 'f7'))
    'SEGW'            = @(, @('tcode', '/nSEGW'))
    'IWFND-MAINT'     = @(, @('tcode', '/n/IWFND/MAINT_SERVICE'))
    'SE38'            = @(, @('tcode', '/nSE38'))
    'SE80'            = @(, @('tcode', '/nSE80'))
    'SM59'            = @(, @('tcode', '/nSM59'))
    'SICF'            = @(, @('tcode', '/nSICF'))
}

$names = @($catalog.Keys)
if ($Screens.Count -gt 0) {
    $Screens = @($Screens | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
    $sel = @()
    foreach ($s in $Screens) {
        $m = @($names | Where-Object { $_ -ieq $s })
        if ($m.Count -eq 0) { Write-Host "Unknown screen '$s'. Known: $($names -join ', ')"; exit 2 }
        $sel += $m[0]
    }
    $names = $sel
}

if (-not $DryRun) {
    foreach ($e in @($OldExe, $NewExe)) { if (-not (Test-Path -LiteralPath $e)) { Write-Host "ERROR: not found: $e"; exit 2 } }
    $a = Invoke-Build $NewExe @('attach', '--session-id', $S)
    if (-not $a.Ok) { Write-Host "ERROR: no SAP GUI session ($S): $($a.Code)"; exit 2 }
    $script:Conn = [string]$a.Json.data.connection_file_id
} else { $script:Conn = '<conn-id>' }

$rows = New-Object System.Collections.ArrayList
$diffs = @{}
$anyDiff = $false
try {
    foreach ($name in $names) {
        if ($DryRun) { Write-Host "PLAN  $name" }
        $navOk = $true; $navMsg = ''
        foreach ($step in $catalog[$name]) {
            $r = Invoke-Build $NewExe $step
            if (-not $r.Ok) { $navOk = $false; $navMsg = "$($step[0]) failed: $($r.Code)"; break }
        }
        if (-not $navOk) {
            Write-Host "SKIP  $name - navigation $navMsg"
            [void]$rows.Add([pscustomobject]@{ Screen = $name; OldMs = '-'; NewMs = '-'; OldEl = '-'; NewEl = '-'; OnlyOld = '-'; OnlyNew = '-'; OldKB = '-'; NewKB = '-' })
            try { [void](Invoke-Build $NewExe @('close')) } catch { }
            continue
        }
        $readArgs = @('screen', 'read', '--no-tabs', '--max-rows', '100', '--output', 'json')
        $best = @{}
        foreach ($pair in @(@('Old', $OldExe), @('New', $NewExe))) {
            $runs = @(1..2 | ForEach-Object { Invoke-Build $pair[1] $readArgs })
            $best[$pair[0]] = $runs | Sort-Object Ms | Select-Object -First 1
        }
        if ($DryRun) { continue }
        $o = $best['Old']; $n = $best['New']
        if (-not $o.Ok -or -not $n.Ok) {
            Write-Host "SKIP  $name - read failed (old=$($o.Code) new=$($n.Code))"
            [void]$rows.Add([pscustomobject]@{ Screen = $name; OldMs = $o.Ms; NewMs = $n.Ms; OldEl = '-'; NewEl = '-'; OnlyOld = '-'; OnlyNew = '-'; OldKB = '-'; NewKB = '-' })
            continue
        }
        $io = Get-Ids $o.Json; $idn = Get-Ids $n.Json
        $onlyOld = @($io | Where-Object { $idn -notcontains $_ })
        $onlyNew = @($idn | Where-Object { $io -notcontains $_ })
        if ($onlyOld.Count -or $onlyNew.Count) { $anyDiff = $true; $diffs[$name] = @{ Old = $onlyOld; New = $onlyNew } }
        [void]$rows.Add([pscustomobject]@{
            Screen = $name; OldMs = $o.Ms; NewMs = $n.Ms; OldEl = $io.Count; NewEl = $idn.Count
            OnlyOld = $onlyOld.Count; OnlyNew = $onlyNew.Count
            OldKB = [math]::Round($o.Raw.Length / 1024.0, 1); NewKB = [math]::Round($n.Raw.Length / 1024.0, 1)
        })
    }
}
finally {
    if (-not $DryRun) { try { [void](Invoke-Build $NewExe @('close')) } catch { }; try { [void](Invoke-Build $NewExe @('tcode', '/n')) } catch { } }
}

if ($DryRun) { Write-Host 'Dry run complete: fairyfly was not invoked.'; exit 0 }

Write-Host ''
$rows | Format-Table Screen, OldMs, NewMs, OldEl, NewEl, OnlyOld, OnlyNew, OldKB, NewKB -AutoSize | Out-String -Width 200 | Write-Host
foreach ($k in $diffs.Keys) {
    Write-Host "DIFF $k"
    if ($diffs[$k].Old.Count) { Write-Host ("  only in OLD: " + (($diffs[$k].Old | Select-Object -First 8) -join ', ')) }
    if ($diffs[$k].New.Count) { Write-Host ("  only in NEW: " + (($diffs[$k].New | Select-Object -First 8) -join ', ')) }
}
if ($anyDiff) { Write-Host 'RESULT: element ids differ between builds'; exit 1 }
Write-Host 'RESULT: element-id sets identical on all compared screens'
exit 0
