<#
.SYNOPSIS
  Sets fairyfly's calendar version in the three files that carry it: src/include/version.h (FAIRYFLY_VERSION, what
  `fairyfly --version` prints), CMakeLists.txt (project VERSION: the exe's version resource) and vcpkg.json.
.DESCRIPTION
  Versions are YYYY.MM.DD, or YYYY.MM.DD.N (N = 1..65535, no leading zero) for the N-th further release on the same
  day: 2026.10.24 < 2026.10.24.1 < 2026.10.24.2 < 2026.10.25. Each file must contain its version line exactly once.
  version.h gets the zero-padded form (2026.10.05, what --version prints and the tag uses); CMakeLists.txt and
  vcpkg.json get the plain numbers (2026.10.5: vcpkg refuses leading zeros).
  Used for the release bump (docs/SIGNING.md) and by .github/workflows/winget.yml, which builds same-day and next-day
  variants of the exe to prove the MSI upgrades.
.PARAMETER Version
  The new version (a leading v is accepted and dropped).
.PARAMETER Root
  Repository root (default: the parent of packaging/).
.EXAMPLE
  ./packaging/set_version.ps1 -Version 2026.10.24.1
#>
param(
    [Parameter(Mandatory)][string]$Version,
    [string]$Root = (Join-Path $PSScriptRoot '..')
)
$ErrorActionPreference = 'Stop'
$Version = $Version -replace '^v', ''
if ($Version -notmatch '^(\d{4})\.(\d{2})\.(\d{2})(?:\.([1-9]\d{0,4}))?$' -or ($Matches[4] -and [int]$Matches[4] -gt 65535)) {
    throw "not a calendar version YYYY.MM.DD or YYYY.MM.DD.N (N 1..65535): $Version"
}
$date = [datetime]::MinValue
if (-not [datetime]::TryParseExact("$($Matches[1])-$($Matches[2])-$($Matches[3])", 'yyyy-MM-dd', [cultureinfo]::InvariantCulture,
        [Globalization.DateTimeStyles]::None, [ref]$date)) { throw "not a date: $Version" }

$plain = (($Version -split '\.') | ForEach-Object { [string][int]$_ }) -join '.'

function Set-Line([string]$relative, [string]$pattern, [string]$replacement) {
    $path = Join-Path $Root $relative
    $text = [IO.File]::ReadAllText($path)
    $hits = [regex]::Matches($text, $pattern, 'Multiline').Count
    if ($hits -ne 1) { throw "${relative}: expected one match of /$pattern/, found $hits" }
    $new = [regex]::Replace($text, $pattern, $replacement, 'Multiline')
    [IO.File]::WriteAllText($path, $new, [Text.UTF8Encoding]::new($false))
    Write-Host "${relative} updated"
}
Set-Line 'src/include/version.h' '(FAIRYFLY_VERSION\s*=\s*")[0-9.]+(")' "`${1}$Version`${2}"
Set-Line 'CMakeLists.txt' '^(project\(fairyfly VERSION )[0-9.]+( )' "`${1}$plain`${2}"
Set-Line 'vcpkg.json' '^(\s*"version"\s*:\s*")[0-9.]+(")' "`${1}$plain`${2}"
