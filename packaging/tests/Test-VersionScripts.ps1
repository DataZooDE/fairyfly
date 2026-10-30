<#
.SYNOPSIS
  Checks the version handling of the packaging scripts (CI: .github/workflows/winget.yml; any pwsh 7, no install):
  packaging/msi_version.ps1 (calendar version -> MSI ProductVersion), packaging/msix_version.ps1 and
  cmake/msix_manifest.cmake (-> MSIX package version), packaging/check_msix_version.ps1 (release.yml's MSIX version
  check), packaging/set_version.ps1 (on a copy of the
  version files) and the version check of packaging/winget/New-WingetManifest.ps1.
.DESCRIPTION
  Versions are YYYY.MM.DD or YYYY.MM.DD.N (N = 1..99) for a further release on the same day. Windows Installer
  compares only the first three fields of a ProductVersion, so the MSI puts N into the third one, DD*100+N:
  2026.10.24 -> 26.10.2400, 2026.10.24.1 -> 26.10.2401 (a same-day downgrade is refused, proven by
  Test-WingetUserInstall.ps1). Up to 2026.10.24.1 the MSIs were 26.10.DD[.N]; every new MSI version is above them.
#>
$ErrorActionPreference = 'Stop'
$packaging = Split-Path $PSScriptRoot -Parent
$failures = 0
function Check([bool]$cond, [string]$msg) {
    if ($cond) { Write-Host "PASS: $msg" } else { Write-Host "FAIL: $msg"; $script:failures++ }
}
function Pass([string]$msg) { Write-Host "PASS: $msg" }
function Try-Run([scriptblock]$block) {
    try { return [pscustomobject]@{ Out = (& $block | Out-String).Trim(); Error = '' } }
    catch { return [pscustomobject]@{ Out = ''; Error = $_.Exception.Message } }
}

Write-Host '=== msi_version.ps1 ==='
$msiVersion = Join-Path $packaging 'msi_version.ps1'
$good = [ordered]@{
    '2026.10.24'        = '26.10.2400'
    'v2026.10.24'       = '26.10.2400'
    '2026.10.05'        = '26.10.500'
    '2026.9.30'         = '26.9.3000'
    '2026.10.24.1'      = '26.10.2401'
    'v2026.10.24.1'     = '26.10.2401'
    '2026.10.24.12'     = '26.10.2412'
    '2026.10.24.99'     = '26.10.2499'
    '2026.10.31.99'     = '26.10.3199'
    '2255.12.31'        = '255.12.3100'
}
foreach ($k in $good.Keys) {
    $r = Try-Run { & $msiVersion -Version $k }
    Check ($r.Out -eq $good[$k]) "msi_version $k -> '$($r.Out)' (expected $($good[$k]))$(if ($r.Error) { " error: $($r.Error)" })"
}
foreach ($bad in '2026.10.24.0', '2026.10.24.01', '2026.10.24.100', '2026.10.24.65535', '2026.10.24.65536', '2026.10.24.', '2026.10.24.1.1', '2026.10.24-1',
                 '2026.10.24.x', '1999.01.01', '2256.01.01', '2026.10', '2026.13.01', '2026.10.32.1', 'vv2026.10.24') {
    $r = Try-Run { & $msiVersion -Version $bad }
    Check ($r.Error -ne '' -and $r.Out -eq '') "msi_version refuses '$bad' ($($r.Error))"
}
# Windows Installer orders products by the first three fields only: every release must be above the one before in
# them, starting from the MSIs released with the old scheme (26.10.23, 26.10.24, 26.10.24.1 = 26.10.24 for MSI)
function Msi3([string]$v) { $p = $v.Split('.'); [version]"$($p[0]).$($p[1]).$($p[2])" }
$order = @('=26.10.23', '=26.10.24', '=26.10.24.1', '2026.10.24.2', '2026.10.24.3', '2026.10.24.99', '2026.10.25', '2026.10.25.1',
           '2026.10.31.99', '2026.11.01', '2026.12.31.99', '2027.01.01')
$prev = $null
foreach ($v in $order) {
    $m = if ($v.StartsWith('=')) { $v.Substring(1) } else { (Try-Run { & $msiVersion -Version $v }).Out }
    if ($prev) {
        $up = (Msi3 $m) -gt (Msi3 $prev.Msi)
        $same = (Msi3 $m) -eq (Msi3 $prev.Msi)
        if ($prev.Old -and $v.StartsWith('=') -and $same) { Pass "old scheme: $m counts as $($prev.Msi) for Windows Installer (the bug)" }
        else { Check $up "MSI $m ($($v.TrimStart('='))) is above $($prev.Msi) ($($prev.V)) in the first three fields: an upgrade, the reverse a downgrade" }
    }
    $prev = [pscustomobject]@{ V = $v.TrimStart('='); Msi = $m; Old = $v.StartsWith('=') }
}

$header = Get-Content (Join-Path $packaging '..\src\include\version.h') -Raw
$null = $header -match 'FAIRYFLY_VERSION\s*=\s*"([0-9.]+)"'
$fromHeader = $Matches[1]
$r = Try-Run { & $msiVersion }
$expected = (Try-Run { & $msiVersion -Version $fromHeader }).Out
Check ($r.Out -ne '' -and $r.Out -eq $expected) "msi_version without -Version reads version.h ($fromHeader -> $($r.Out))"

Write-Host "`n=== msix_version.ps1 and cmake/msix_manifest.cmake (MSIX package version) ==="
# YYYY.MM.(DD*100+N).0: four fields, the Store wants the fourth 0; the same N slot as the MSI
$msixVersion = Join-Path $packaging 'msix_version.ps1'
$msixCmake = Join-Path $packaging '..\cmake\msix_manifest.cmake'
function Invoke-MsixCMake([string]$v) {
    $out = & cmake "-DVERSION=$v" -P $msixCmake 2>&1 | Out-String
    $m = [regex]::Match($out, 'msix version (\S+)')
    [pscustomobject]@{ Ok = ($LASTEXITCODE -eq 0); Version = $(if ($m.Success) { $m.Groups[1].Value } else { '' }); Out = $out.Trim() }
}
$goodMsix = [ordered]@{
    '2026.10.24'    = '2026.10.2400.0'
    'v2026.10.24'   = '2026.10.2400.0'
    '2026.10.05'    = '2026.10.500.0'
    '2026.9.30'     = '2026.9.3000.0'
    '2026.10.24.1'  = '2026.10.2401.0'
    '2026.10.24.2'  = '2026.10.2402.0'
    '2026.10.31'    = '2026.10.3100.0'
    '2026.10.31.99' = '2026.10.3199.0'
    'v2026.11.01'   = '2026.11.100.0'
    '2026.11.01.1'  = '2026.11.101.0'
    '2026.12.31.99' = '2026.12.3199.0'
    'v2027.01.01'   = '2027.1.100.0'
    '2028.02.29'    = '2028.2.2900.0'
    '2255.12.31'    = '2255.12.3100.0'
}
foreach ($k in $goodMsix.Keys) {
    $r = Try-Run { & $msixVersion -Version $k }
    Check ($r.Out -eq $goodMsix[$k]) "msix_version $k -> '$($r.Out)' (expected $($goodMsix[$k]))$(if ($r.Error) { " error: $($r.Error)" })"
    if (-not $k.StartsWith('v')) {   # CMake takes the plain version only (project VERSION)
        $c = Invoke-MsixCMake $k
        Check ($c.Ok -and $c.Version -eq $goodMsix[$k]) "cmake fairyfly_msix_version $k -> '$($c.Version)' (expected $($goodMsix[$k]))"
    }
}
foreach ($bad in '2026.10.24.0', '2026.10.24.100', '2026.02.30', '2026.10', 'V2026.10.24', '1999.12.31') {
    $r = Try-Run { & $msixVersion -Version $bad }
    Check ($r.Error -ne '' -and $r.Out -eq '') "msix_version refuses '$bad' ($($r.Error))"
}
# the package version must rise with every release, the same-day ones included (the Store refuses a lower one)
$prev = $null
foreach ($v in '2026.10.24', '2026.10.24.1', '2026.10.24.2', '2026.10.24.99', '2026.10.25', '2026.10.28', '2026.10.28.1', '2026.10.31',
               '2026.10.31.99', '2026.11.01', '2026.11.01.1', '2026.11.30.99', '2026.12.01', '2026.12.31.99', '2027.01.01', '2027.02.28.99',
               '2027.03.01') {
    $cur = [version](Try-Run { & $msixVersion -Version $v }).Out
    if ($prev) { Check ($cur -gt $prev.Msix) "MSIX $cur ($v) is above $($prev.Msix) ($($prev.V))" }
    $prev = [pscustomobject]@{ V = $v; Msix = $cur }
}
$r = Try-Run { & $msixVersion }
Check ($r.Out -ne '' -and $r.Out -eq (Try-Run { & $msixVersion -Version $fromHeader }).Out) "msix_version without -Version reads version.h ($fromHeader -> $($r.Out))"

Write-Host "`n=== check_msix_version.ps1 (release.yml: the packed MSIX version against the tag and the previous releases) ==="
# release.yml (job build-msix) and build.yml (build-windows-msix, release dry run): the package version must be
# msix_version(<tag>) and strictly above the MSIX version of every earlier release (Partner Center refuses a package
# whose version is not higher than the last one submitted)
$msixCheck = Join-Path $packaging 'check_msix_version.ps1'
$released = @('v2026.10.05', 'v2026.10.23', 'v2026.10.24', 'v2026.10.24.1', 'v2026.10.24.2', 'v2026.10.25', 'v2026.10.25.1',
              'v2026.10.25.2', 'v2026.10.27', 'v2026.10.28')
function Invoke-MsixCheck([string]$tag, [string]$msix, [string[]]$prev) {
    Try-Run { & $msixCheck -Tag $tag -MsixVersion $msix -PreviousTags $prev }
}
foreach ($case in @(
        @('v2026.10.29', '2026.10.2900.0', $released, 'next day'),
        @('v2026.10.28.1', '2026.10.2801.0', $released, 'same day .1 after the plain day'),
        @('v2026.10.28.2', '2026.10.2802.0', ($released + 'v2026.10.28.1'), 'same day .2 after .1'),
        @('v2026.10.28.10', '2026.10.2810.0', ($released + 'v2026.10.28.9'), 'same day .10 after .9 (numeric, not text order)'),
        @('v2026.11.01', '2026.11.100.0', ($released + 'v2026.10.31.99'), 'month boundary after the last same-day release'),
        @('v2027.01.01', '2027.1.100.0', ($released + 'v2026.12.31.99'), 'year boundary'),
        @('v2026.10.28', '2026.10.2800.0', $released, 'a re-run on a released tag compares with the releases before it'),
        @('v2026.10.29', '2026.10.2900.0', @(), 'no earlier release'),
        @('v2026.10.29', '2026.10.2900.0', ($released + 'nightly' + 'v1.0.0' + ''), 'tags that are no calendar version are ignored'))) {
    $r = Invoke-MsixCheck $case[0] $case[1] $case[2]
    Check ($r.Error -eq '') "check_msix_version accepts $($case[0]) = $($case[1]): $($case[3])$(if ($r.Error) { " error: $($r.Error)" })"
}
foreach ($case in @(
        @('v2026.10.29', '2026.10.2901.0', $released, 'package version is not the tag''s'),
        @('v2026.10.29.1', '2026.10.2900.0', $released, 'same-day N missing in the package version'),
        @('v2026.10.29', '2026.10.29.0', $released, 'old-style version'),
        @('v2026.10.29', '2026.10.2900', $released, 'three fields'),
        @('v2026.10.29', '2026.10.2900.1', $released, 'fourth field not 0'),
        @('v2026.10.29', '', $released, 'empty package version'),
        @('V2026.10.29', '2026.10.2900.0', $released, 'uppercase V'),
        @('2026.10.29', '2026.10.2900.0', $released, 'no v'),
        @('v2026.10.29.0', '2026.10.2900.0', $released, 'N = 0'),
        @('v2026.10.27.1', '2026.10.2701.0', $released, 'below the latest release v2026.10.28'),
        @('v2026.10.28.1', '2026.10.2801.0', ($released + 'v2026.10.28.2'), 'same day .1 after .2'),
        @('v2026.10.28.9', '2026.10.2809.0', ($released + 'v2026.10.28.10'), 'same day .9 after .10'),
        @('v2026.10.31.99', '2026.10.3199.0', ($released + 'v2026.11.01'), 'last day of a month after the first of the next'),
        @('v2026.12.31', '2026.12.3100.0', ($released + 'v2027.01.01'), 'year boundary backwards'),
        @('v2026.10.09', '2026.10.900.0', @('v2026.10.05', 'v2026.10.9'), 'equal to an earlier release (not strictly above)'))) {
    $r = Invoke-MsixCheck $case[0] $case[1] $case[2]
    Check ($r.Error -ne '') "check_msix_version refuses $($case[0]) = '$($case[1])': $($case[3]) ($($r.Error))"
}
$r = Invoke-MsixCheck 'v2026.10.27.1' '2026.10.2701.0' $released
Check ($r.Error -match 'v2026\.10\.28' -and $r.Error -match '2026\.10\.2800\.0') "the refusal names the newer release and its MSIX version ($($r.Error))"
$r = Invoke-MsixCheck 'v2026.10.29' '2026.10.2900.0' $released
Check ($r.Out -match '2026\.10\.2900\.0' -and $r.Out -match 'v2026\.10\.28') "the check prints the version and the previous release ($($r.Out))"

Write-Host "`n=== set_version.ps1 (on a copy) ==="
$tmp = Join-Path ([IO.Path]::GetTempPath()) ("ffy-setversion-" + [guid]::NewGuid())
New-Item -ItemType Directory -Force (Join-Path $tmp 'src/include') | Out-Null
$repo = Join-Path $packaging '..'
foreach ($f in 'src/include/version.h', 'CMakeLists.txt', 'vcpkg.json') { Copy-Item (Join-Path $repo $f) (Join-Path $tmp $f) }
$setVersion = Join-Path $packaging 'set_version.ps1'
function Read-Versions {
    $h = Get-Content (Join-Path $tmp 'src/include/version.h') -Raw
    $c = Get-Content (Join-Path $tmp 'CMakeLists.txt') -Raw
    $j = Get-Content (Join-Path $tmp 'vcpkg.json') -Raw | ConvertFrom-Json
    $null = $h -match 'FAIRYFLY_VERSION\s*=\s*"([0-9.]+)"'; $hv = $Matches[1]
    $null = $c -match '(?m)^project\(fairyfly VERSION ([0-9.]+) '; $cv = $Matches[1]
    "$hv|$cv|$($j.version)"
}
try {
    # version.h zero padded (what --version prints), CMakeLists.txt and vcpkg.json plain (vcpkg refuses leading zeros)
    foreach ($case in @('2026.10.24.1', '2026.10.24.1'), @('v2026.10.24.2', '2026.10.24.2'), @('2026.10.05', '2026.10.5'),
                      @('2026.09.05.3', '2026.9.5.3'), @('2026.10.24', '2026.10.24')) {
        $v = $case[0]; $plain = $case[1]
        $r = Try-Run { & $setVersion -Version $v -Root $tmp }
        $want = $v.TrimStart('v')
        Check ($r.Error -eq '' -and (Read-Versions) -eq "$want|$plain|$plain") "set_version $v -> version.h|CMakeLists.txt|vcpkg.json = $(Read-Versions)$(if ($r.Error) { " error: $($r.Error)" })"
    }
    $before = Read-Versions
    foreach ($bad in '2026.10.24.0', '2026.10.24.100', '2026.10.24.65536', '2026.02.30', '2026.10.24-1', '26.10.24') {
        $r = Try-Run { & $setVersion -Version $bad -Root $tmp }
        Check ($r.Error -ne '' -and (Read-Versions) -eq $before) "set_version refuses '$bad' and changes nothing ($($r.Error))"
    }
    # a file without its version line: nothing is written, not even the files checked before it
    $cm = Join-Path $tmp 'CMakeLists.txt'
    $saved = [IO.File]::ReadAllText($cm)
    [IO.File]::WriteAllText($cm, ($saved -replace 'project\(fairyfly VERSION', 'project(fairyfly  VERSION'))
    $r = Try-Run { & $setVersion -Version '2026.10.24.7' -Root $tmp }
    [IO.File]::WriteAllText($cm, $saved)
    Check ($r.Error -match 'nothing was changed' -and (Read-Versions) -eq $before) "set_version changes no file when one version line is missing ($($r.Error))"
    $other = Get-Content (Join-Path $tmp 'vcpkg.json') -Raw
    Check ($other -match '"builtin-baseline"') 'vcpkg.json keeps its other keys'
} finally { Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue }

Write-Host "`n=== New-WingetManifest.ps1 version check ==="
# the version is checked before the MSI is opened: a good version fails later (no such MSI), a bad one at once
$generator = Join-Path $packaging 'winget\New-WingetManifest.ps1'
$noMsi = Join-Path ([IO.Path]::GetTempPath()) 'ffy-no-such.msi'
$outDir = Join-Path ([IO.Path]::GetTempPath()) 'ffy-no-manifest'
foreach ($v in 'v2026.10.24', '2026.10.24.1', 'v2026.10.24.1', '2026.10.24.99') {
    $r = Try-Run { & $generator -Version $v -MsiPath $noMsi -OutDir $outDir }
    Check ($r.Error -ne '' -and $r.Error -notmatch 'not a calendar version') "New-WingetManifest accepts the version $v (then: $($r.Error))"
}
foreach ($bad in '2026.10.24.0', '2026.10.24.01', '2026.10.24.100', 'v2026.10.24.1.1', '2026.10.5') {
    $r = Try-Run { & $generator -Version $bad -MsiPath $noMsi -OutDir $outDir }
    Check ($r.Error -match 'not a calendar version') "New-WingetManifest refuses the version $bad ($($r.Error))"
}

Write-Host "`n=== check_release_tag.ps1 (release.yml's tag check) ==="
# the updater (src/update/update_model.cpp parse_release) only takes a lowercase v: the check must be case-sensitive;
# N is 1..99 as for set_version/msi_version (MSI (YYYY-2000).MM.(DD*100+N)), though the updater still reads up to 65535
$tagCheck = Join-Path $packaging 'check_release_tag.ps1'
foreach ($case in @('v2026.10.24', '2026.10.24'), @('v2026.10.24.1', '2026.10.24.1'), @('v2026.10.24.99', '2026.10.24.99')) {
    $r = Try-Run { & $tagCheck -Tag $case[0] -Version $case[1] }
    Check ($r.Error -eq '') "check_release_tag accepts $($case[0]) for $($case[1])$(if ($r.Error) { " error: $($r.Error)" })"
}
foreach ($case in @('V2026.10.24.1', '2026.10.24.1'), @('V2026.10.24', '2026.10.24'), @('v2026.10.24', '2026.10.24.1'),
                  @('v2026.10.24.1', '2026.10.24'), @('2026.10.24', '2026.10.24'), @('v2026.10.24.0', '2026.10.24.0'),
                  @('v2026.10.24.100', '2026.10.24.100'), @('v2026.10.24.65535', '2026.10.24.65535'),
                  @('v2026.10.24.65536', '2026.10.24.65536'), @('vv2026.10.24', 'v2026.10.24'), @('v2026.10.24 ', '2026.10.24'),
                  @('', '2026.10.24')) {
    $r = Try-Run { & $tagCheck -Tag $case[0] -Version $case[1] }
    Check ($r.Error -ne '') "check_release_tag refuses '$($case[0])' for $($case[1]) ($($r.Error))"
}
# a tag with an uppercase V is no tag the updater takes: msi_version.ps1 and New-WingetManifest.ps1 refuse it too
foreach ($bad in 'V2026.10.24', 'V2026.10.24.1') {
    $r = Try-Run { & $msiVersion -Version $bad }
    Check ($r.Error -ne '' -and $r.Out -eq '') "msi_version refuses '$bad' ($($r.Error))"
    $r = Try-Run { & $generator -Version $bad -MsiPath $noMsi -OutDir $outDir }
    Check ($r.Error -match 'not a calendar version') "New-WingetManifest refuses the version $bad ($($r.Error))"
}

Write-Host "`n=== cmake/calendar_version.cmake (what configuring checks in CMakeLists.txt) ==="
$cmakeCheck = Join-Path $packaging '..\cmake\calendar_version.cmake'
function Invoke-CMakeCheck([string]$v) {
    $out = & cmake "-DVERSION=$v" -P $cmakeCheck 2>&1 | Out-String
    [pscustomobject]@{ Ok = ($LASTEXITCODE -eq 0); Out = $out.Trim() }
}
foreach ($v in '2026.10.24', '2026.10.5', '2026.10.05', '2026.10.24.1', '2026.10.24.99', '2024.2.29', '2000.2.29', '2026.12.31', '2026.1.31', '2026.4.30') {
    $r = Invoke-CMakeCheck $v
    Check $r.Ok "configure accepts the version $v$(if (-not $r.Ok) { ": $($r.Out)" })"
}
foreach ($v in '2026.2.30', '2026.02.30', '2026.2.29', '1900.2.29', '2100.2.29', '2026.4.31', '2026.6.31', '2026.9.31', '2026.11.31',
               '2026.1.32', '2026.13.1', '2026.0.1', '2026.1.0', '2026.00.10', '1999.12.31', '2026.10.24.0', '2026.10.24.100', '2026.10.24.65535', '2026.10.24.65536',
               '2026.10.24.01', '2026.10', '26.10.24', '2026.10.24-1', 'v2026.10.24') {
    $r = Invoke-CMakeCheck $v
    Check (-not $r.Ok -and $r.Out -match 'calendar version|not a date|1\.\.99') "configure refuses the version $v ($(($r.Out -split "`n")[-1]))"
}

if ($failures) { Write-Host "`n$failures check(s) FAILED"; exit 1 }
Write-Host "`nALL VERSION SCRIPT CHECKS PASSED"
exit 0   # not the exit code of the last cmake -P run above (an expected refusal)
