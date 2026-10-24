<#
.SYNOPSIS
  Checks the version handling of the packaging scripts (CI: .github/workflows/winget.yml; any pwsh 7, no install):
  packaging/msi_version.ps1 (calendar version -> MSI ProductVersion), packaging/set_version.ps1 (on a copy of the
  version files) and the version check of packaging/winget/New-WingetManifest.ps1.
.DESCRIPTION
  Versions are YYYY.MM.DD or YYYY.MM.DD.N (N = 1..65535) for a further release on the same day. The MSI keeps N as
  its fourth field: 2026.10.24.1 -> 26.10.24.1 (Windows Installer ignores that field when it compares versions; the
  package's AllowSameVersionUpgrades makes 26.10.24.1 replace 26.10.24, proven by Test-WingetUserInstall.ps1).
#>
$ErrorActionPreference = 'Stop'
$packaging = Split-Path $PSScriptRoot -Parent
$failures = 0
function Check([bool]$cond, [string]$msg) {
    if ($cond) { Write-Host "PASS: $msg" } else { Write-Host "FAIL: $msg"; $script:failures++ }
}
function Try-Run([scriptblock]$block) {
    try { return [pscustomobject]@{ Out = (& $block | Out-String).Trim(); Error = '' } }
    catch { return [pscustomobject]@{ Out = ''; Error = $_.Exception.Message } }
}

Write-Host '=== msi_version.ps1 ==='
$msiVersion = Join-Path $packaging 'msi_version.ps1'
$good = [ordered]@{
    '2026.10.24'        = '26.10.24'
    'v2026.10.24'       = '26.10.24'
    '2026.10.05'        = '26.10.5'
    '2026.9.30'         = '26.9.30'
    '2026.10.24.1'      = '26.10.24.1'
    'v2026.10.24.1'     = '26.10.24.1'
    '2026.10.24.12'     = '26.10.24.12'
    '2026.10.24.65535'  = '26.10.24.65535'
    '2255.12.31'        = '255.12.31'
}
foreach ($k in $good.Keys) {
    $r = Try-Run { & $msiVersion -Version $k }
    Check ($r.Out -eq $good[$k]) "msi_version $k -> '$($r.Out)' (expected $($good[$k]))$(if ($r.Error) { " error: $($r.Error)" })"
}
foreach ($bad in '2026.10.24.0', '2026.10.24.01', '2026.10.24.65536', '2026.10.24.', '2026.10.24.1.1', '2026.10.24-1',
                 '2026.10.24.x', '1999.01.01', '2256.01.01', '2026.10', '2026.13.01', '2026.10.32.1', 'vv2026.10.24') {
    $r = Try-Run { & $msiVersion -Version $bad }
    Check ($r.Error -ne '' -and $r.Out -eq '') "msi_version refuses '$bad' ($($r.Error))"
}
$header = Get-Content (Join-Path $packaging '..\src\include\version.h') -Raw
$null = $header -match 'FAIRYFLY_VERSION\s*=\s*"([0-9.]+)"'
$fromHeader = $Matches[1]
$r = Try-Run { & $msiVersion }
$expected = (Try-Run { & $msiVersion -Version $fromHeader }).Out
Check ($r.Out -ne '' -and $r.Out -eq $expected) "msi_version without -Version reads version.h ($fromHeader -> $($r.Out))"

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
    foreach ($bad in '2026.10.24.0', '2026.10.24.65536', '2026.02.30', '2026.10.24-1', '26.10.24') {
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
foreach ($v in 'v2026.10.24', '2026.10.24.1', 'v2026.10.24.1') {
    $r = Try-Run { & $generator -Version $v -MsiPath $noMsi -OutDir $outDir }
    Check ($r.Error -ne '' -and $r.Error -notmatch 'not a calendar version') "New-WingetManifest accepts the version $v (then: $($r.Error))"
}
foreach ($bad in '2026.10.24.0', '2026.10.24.01', 'v2026.10.24.1.1', '2026.10.5') {
    $r = Try-Run { & $generator -Version $bad -MsiPath $noMsi -OutDir $outDir }
    Check ($r.Error -match 'not a calendar version') "New-WingetManifest refuses the version $bad ($($r.Error))"
}

Write-Host "`n=== check_release_tag.ps1 (release.yml's tag check) ==="
# the updater (src/update/update_model.cpp parse_release) only takes a lowercase v: the check must be case-sensitive
$tagCheck = Join-Path $packaging 'check_release_tag.ps1'
foreach ($case in @('v2026.10.24', '2026.10.24'), @('v2026.10.24.1', '2026.10.24.1'), @('v2026.10.24.65535', '2026.10.24.65535')) {
    $r = Try-Run { & $tagCheck -Tag $case[0] -Version $case[1] }
    Check ($r.Error -eq '') "check_release_tag accepts $($case[0]) for $($case[1])$(if ($r.Error) { " error: $($r.Error)" })"
}
foreach ($case in @('V2026.10.24.1', '2026.10.24.1'), @('V2026.10.24', '2026.10.24'), @('v2026.10.24', '2026.10.24.1'),
                  @('v2026.10.24.1', '2026.10.24'), @('2026.10.24', '2026.10.24'), @('v2026.10.24.0', '2026.10.24.0'),
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

if ($failures) { Write-Host "`n$failures check(s) FAILED"; exit 1 }
Write-Host "`nALL VERSION SCRIPT CHECKS PASSED"
