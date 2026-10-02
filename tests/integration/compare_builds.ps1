<#
.SYNOPSIS
  Compare two fairyfly builds on a fixed list of read-only Bigfox (SAP A4H) screens.

.DESCRIPTION
  For each screen: navigate ONCE with NewExe, then run `screen read --no-tabs --max-rows 100 --output json`
  with both builds (best of 2 runs each), compare the sorted element-id sets and print
  Screen / OldMs / NewMs / OldEl / NewEl / OnlyOld / OnlyNew / OldKB / NewKB.
  Prints the first 8 differing ids per screen. Exit code 1 if any id differs, 2 if no SAP session, else 0.

  With -FullCompare each screen is additionally deep-compared: the parsed JSON of data.elements (matched by id),
  data.hierarchy, data.tabs and data.status_bar must be identical between the builds after duration_ms and
  timestamp fields (timestamp, *_timestamp, *_at) are stripped. The read then runs without --no-tabs so that
  data.tabs is populated (this also adds the tab elements to the id sets). Every difference is printed as a
  path-level diff (up to 20 per screen) and makes the exit code 1. Without the switch nothing changes.

  Prerequisites: logged-in Bigfox session /app/con[0]/ses[0] on SAP Easy Access, display authorizations.
  Only selection-screen search values are filled (TADIR, DEVELOPER, rdisp/wp_no_dia, '*', a date).
  Never presses Save/Delete/Release/Stop/Create/Change. Ends with `tcode /n`.

.PARAMETER OldExe  Baseline fairyfly.exe.
.PARAMETER NewExe  Candidate fairyfly.exe (also used for navigation).
.PARAMETER Screens Optional subset of screen names (see the list below; matching is case-insensitive).
.PARAMETER DryRun  Print the navigation plan only.
.PARAMETER FullCompare  Also deep-compare data.elements (by id), hierarchy, tabs and status_bar of both builds.
#>
param(
    [Parameter(Mandatory = $true)][string]$OldExe,
    [Parameter(Mandatory = $true)][string]$NewExe,
    [string[]]$Screens = @(),
    [switch]$DryRun,
    [switch]$FullCompare
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

# --- -FullCompare helpers --------------------------------------------------------------------------------------
# Volatile keys that legitimately differ between two runs of the same screen.
function Test-VolatileKey([string]$Key) {
    return ($Key -ceq 'duration_ms' -or $Key -ceq 'timestamp' -or $Key -clike '*_timestamp' -or $Key -clike '*_at')
}

# Deep copy of a ConvertFrom-Json tree as ordered dictionaries / object[] / scalars, without volatile keys.
function Remove-VolatileFields($Node) {
    if ($null -eq $Node) { return $null }
    if ($Node -is [System.Management.Automation.PSCustomObject]) {
        $d = [ordered]@{}
        foreach ($p in $Node.PSObject.Properties) {
            if (-not (Test-VolatileKey $p.Name)) { $d[$p.Name] = Remove-VolatileFields $p.Value }
        }
        return $d
    }
    if ($Node -is [System.Collections.IList]) {
        $items = New-Object System.Collections.ArrayList
        foreach ($i in $Node) { [void]$items.Add((Remove-VolatileFields $i)) }
        return , ($items.ToArray())
    }
    return $Node
}

function Format-JsonValue($Node) {
    $t = if ($null -eq $Node) { 'null' } else { ConvertTo-Json -InputObject $Node -Compress -Depth 20 }
    if ($t.Length -gt 80) { $t = $t.Substring(0, 77) + '...' }
    return $t
}

# Appends one readable line per difference ("path: old <> new") to $Out. Arrays whose items are all objects with
# unique ids on both sides are matched by id (order-independent), other arrays by index.
function Compare-JsonTree($Old, $New, [string]$Path, [System.Collections.IList]$Out) {
    $oDict = $Old -is [System.Collections.IDictionary]; $nDict = $New -is [System.Collections.IDictionary]
    $oArr = ($Old -is [object[]]); $nArr = ($New -is [object[]])
    if ($oDict -and $nDict) {
        $keys = @($Old.Keys) + @($New.Keys | Where-Object { -not $Old.Contains($_) })
        foreach ($k in $keys) {
            $p = if ($Path) { "$Path.$k" } else { [string]$k }
            if (-not $New.Contains($k)) { [void]$Out.Add("${p}: only in OLD = $(Format-JsonValue $Old[$k])") }
            elseif (-not $Old.Contains($k)) { [void]$Out.Add("${p}: only in NEW = $(Format-JsonValue $New[$k])") }
            else { Compare-JsonTree $Old[$k] $New[$k] $p $Out }
        }
        return
    }
    if ($oArr -and $nArr) {
        $keyed = $false
        $oIds = @(); $nIds = @()
        if ($Old.Count -gt 0 -and $New.Count -gt 0) {
            $oIds = @($Old | ForEach-Object { if ($_ -is [System.Collections.IDictionary] -and $_.Contains('id') -and $null -ne $_['id']) { [string]$_['id'] } else { $null } })
            $nIds = @($New | ForEach-Object { if ($_ -is [System.Collections.IDictionary] -and $_.Contains('id') -and $null -ne $_['id']) { [string]$_['id'] } else { $null } })
            $keyed = (-not ($oIds -contains $null)) -and (-not ($nIds -contains $null)) -and
                     (@($oIds | Sort-Object -Unique).Count -eq $oIds.Count) -and (@($nIds | Sort-Object -Unique).Count -eq $nIds.Count)
        }
        if ($keyed) {
            for ($i = 0; $i -lt $Old.Count; $i++) {
                $j = [array]::IndexOf($nIds, $oIds[$i])
                $p = "$Path[id=$($oIds[$i])]"
                if ($j -lt 0) { [void]$Out.Add("${p}: only in OLD") } else { Compare-JsonTree $Old[$i] $New[$j] $p $Out }
            }
            for ($j = 0; $j -lt $New.Count; $j++) {
                if ([array]::IndexOf($oIds, $nIds[$j]) -lt 0) { [void]$Out.Add("$Path[id=$($nIds[$j])]: only in NEW") }
            }
            return
        }
        if ($Old.Count -ne $New.Count) { [void]$Out.Add("${Path}: length OLD=$($Old.Count) NEW=$($New.Count)") }
        $n = [math]::Min($Old.Count, $New.Count)
        for ($i = 0; $i -lt $n; $i++) { Compare-JsonTree $Old[$i] $New[$i] "$Path[$i]" $Out }
        return
    }
    if ($oDict -or $nDict -or $oArr -or $nArr) {
        [void]$Out.Add("${Path}: type differs, OLD = $(Format-JsonValue $Old) NEW = $(Format-JsonValue $New)")
        return
    }
    # Scalars: compare as compact JSON so that 1 and "1" differ, but 1 and 1.0 from the same parser do not.
    $a = Format-JsonValue $Old; $b = Format-JsonValue $New
    if ($a -cne $b) { [void]$Out.Add("${Path}: OLD = $a <> NEW = $b") }
}

# The parts of a `screen read` result that -FullCompare checks, volatile fields removed, elements sorted by id.
function Get-FullCompareView($Json) {
    $view = [ordered]@{}
    $data = if ($null -ne $Json) { $Json.data } else { $null }
    foreach ($k in @('elements', 'hierarchy', 'tabs', 'status_bar')) {
        $v = $null
        if ($null -ne $data -and $data.PSObject.Properties[$k]) { $v = Remove-VolatileFields $data.$k }
        if ($k -eq 'elements' -and $v -is [object[]]) {
            $v = @($v | Sort-Object { if ($_ -is [System.Collections.IDictionary] -and $_.Contains('id')) { [string]$_['id'] } else { '' } }, { Format-JsonValue $_ })
        }
        $view[$k] = $v
    }
    return $view
}

# Navigation steps are argument arrays run with NewExe. Only selection-screen values are filled.
$catalog = [ordered]@{
    'EasyAccess'      = @(, @('transaction', 'start', '/n'))
    'ST22-selection'  = @(, @('transaction', 'start', '/nST22'))
    'ST22-list'       = @(@('transaction', 'start', '/nST22'), @('element', 'click', "$U/btnTODAY"))
    'SM37-selection'  = @(, @('transaction', 'start', '/nSM37'))
    'SM37-joblist'    = @(@('transaction', 'start', '/nSM37'), @('element', 'fill', "$U/txtBTCH2170-USERNAME", '*'), @('element', 'fill', "$U/ctxtBTCH2170-FROM_DATE", '01.01.2026'), @('element', 'click', "$S/wnd[0]/tbar[1]/btn[8]"))
    'RZ11-detail'     = @(@('transaction', 'start', '/nRZ11'), @('element', 'fill', "$U/ctxtTPFYSTRUCT-NAME", 'rdisp/wp_no_dia'), @('element', 'click', "$U/btnPANZEIGEN_1000"))
    'SU01-selection'  = @(, @('transaction', 'start', '/nSU01'))
    'SU01-display'    = @(@('transaction', 'start', '/nSU01'), @('element', 'fill', "$U/ctxtSUID_ST_BNAME-BNAME", 'DEVELOPER'), @('element', 'click', "$S/wnd[0]/tbar[1]/btn[7]"))
    'SM50'            = @(, @('transaction', 'start', '/nSM50'))
    'RZ04'            = @(, @('transaction', 'start', '/nRZ04'))
    'SE16-TADIR'      = @(@('transaction', 'start', '/nSE16'), @('element', 'fill', "$U/ctxtDATABROWSE-TABLENAME", 'TADIR'), @('key', 'send', 'f7'))
    'SE11-TADIR'      = @(@('transaction', 'start', '/nSE11'), @('element', 'fill', "$U/ctxtRSRD1-TBMA_VAL", 'TADIR'), @('key', 'send', 'f7'))
    'SEGW'            = @(, @('transaction', 'start', '/nSEGW'))
    'IWFND-MAINT'     = @(, @('transaction', 'start', '/n/IWFND/MAINT_SERVICE'))
    'SE38'            = @(, @('transaction', 'start', '/nSE38'))
    'SE80'            = @(, @('transaction', 'start', '/nSE80'))
    'SM59'            = @(, @('transaction', 'start', '/nSM59'))
    'SICF'            = @(, @('transaction', 'start', '/nSICF'))
}

$names = @($catalog.Keys)
if ($Screens.Count -gt 0) {
    $Screens = @($Screens | ForEach-Object { $_ -split ',' } | Where-Object { $_ })
    $sel = @()
    foreach ($want in $Screens) {   # not $s: PowerShell variables are case-insensitive and $S is the session id
        $m = @($names | Where-Object { $_ -ieq $want })
        if ($m.Count -eq 0) { Write-Host "Unknown screen '$want'. Known: $($names -join ', ')"; exit 2 }
        $sel += $m[0]
    }
    $names = $sel
}

if (-not $DryRun) {
    foreach ($e in @($OldExe, $NewExe)) { if (-not (Test-Path -LiteralPath $e)) { Write-Host "ERROR: not found: $e"; exit 2 } }
    $a = Invoke-Build $NewExe @('session', 'attach', '--session-id', $S)
    if (-not $a.Ok) { Write-Host "ERROR: no SAP GUI session ($S): $($a.Code)"; exit 2 }
    $script:Conn = [string]$a.Json.data.connection_file_id
} else { $script:Conn = '<conn-id>' }

$rows = New-Object System.Collections.ArrayList
$diffs = @{}
$fullDiffs = [ordered]@{}
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
            try { [void](Invoke-Build $NewExe @('popup', 'close')) } catch { }
            continue
        }
        $readArgs = @('screen', 'read', '--no-tabs', '--max-rows', '100', '--output', 'json')
        if ($FullCompare) { $readArgs = @('screen', 'read', '--max-rows', '100', '--output', 'json') }  # tabs are compared too
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
        if ($FullCompare) {
            $lines = New-Object System.Collections.ArrayList
            Compare-JsonTree (Get-FullCompareView $o.Json) (Get-FullCompareView $n.Json) '' $lines
            if ($lines.Count) { $anyDiff = $true; $fullDiffs[$name] = $lines }
        }
        [void]$rows.Add([pscustomobject]@{
            Screen = $name; OldMs = $o.Ms; NewMs = $n.Ms; OldEl = $io.Count; NewEl = $idn.Count
            OnlyOld = $onlyOld.Count; OnlyNew = $onlyNew.Count
            OldKB = [math]::Round($o.Raw.Length / 1024.0, 1); NewKB = [math]::Round($n.Raw.Length / 1024.0, 1)
        })
    }
}
finally {
    if (-not $DryRun) { try { [void](Invoke-Build $NewExe @('popup', 'close')) } catch { }; try { [void](Invoke-Build $NewExe @('transaction', 'start', '/n')) } catch { } }
}

if ($DryRun) { Write-Host 'Dry run complete: fairyfly was not invoked.'; exit 0 }

Write-Host ''
$rows | Format-Table Screen, OldMs, NewMs, OldEl, NewEl, OnlyOld, OnlyNew, OldKB, NewKB -AutoSize | Out-String -Width 200 | Write-Host
foreach ($k in $diffs.Keys) {
    Write-Host "DIFF $k"
    if ($diffs[$k].Old.Count) { Write-Host ("  only in OLD: " + (($diffs[$k].Old | Select-Object -First 8) -join ', ')) }
    if ($diffs[$k].New.Count) { Write-Host ("  only in NEW: " + (($diffs[$k].New | Select-Object -First 8) -join ', ')) }
}
foreach ($k in $fullDiffs.Keys) {
    $lines = $fullDiffs[$k]
    Write-Host "FULLDIFF $k ($($lines.Count) differences)"
    foreach ($l in ($lines | Select-Object -First 20)) { Write-Host "  $l" }
    if ($lines.Count -gt 20) { Write-Host "  ... $($lines.Count - 20) more" }
}
if ($anyDiff) {
    if ($diffs.Count) { Write-Host 'RESULT: element ids differ between builds' }
    if ($fullDiffs.Count) { Write-Host 'RESULT: full compare (elements, hierarchy, tabs, status_bar) differs between builds' }
    exit 1
}
if ($FullCompare) { Write-Host 'RESULT: element ids and full screen content (elements, hierarchy, tabs, status_bar) identical on all compared screens'; exit 0 }
Write-Host 'RESULT: element-id sets identical on all compared screens'
exit 0
