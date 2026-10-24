<#
.SYNOPSIS
  Sets fairyfly's calendar version in the three files that carry it: src/include/version.h (FAIRYFLY_VERSION, what
  `fairyfly --version` prints), CMakeLists.txt (project VERSION: the exe's version resource) and vcpkg.json.
.DESCRIPTION
  Versions are YYYY.MM.DD, or YYYY.MM.DD.N (N = 1..99, no leading zero; the MSI version holds DD*100+N) for the N-th further release on the same
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
$Version = $Version -replace '^[vV]', ''
if ($Version -notmatch '^(\d{4})\.(\d{2})\.(\d{2})(?:\.([1-9]\d?))?$') {
    throw "not a calendar version YYYY.MM.DD or YYYY.MM.DD.N (N 1..99): $Version"
}
$date = [datetime]::MinValue
if (-not [datetime]::TryParseExact("$($Matches[1])-$($Matches[2])-$($Matches[3])", 'yyyy-MM-dd', [cultureinfo]::InvariantCulture,
        [Globalization.DateTimeStyles]::None, [ref]$date)) { throw "not a date: $Version" }

$plain = (($Version -split '\.') | ForEach-Object { [string][int]$_ }) -join '.'

# all three checked first, then written: a file whose version line is not found leaves every file unchanged
$edits = foreach ($e in @(
        @{ File = 'src/include/version.h'; Pattern = '(FAIRYFLY_VERSION\s*=\s*")[0-9.]+(")'; Value = $Version },
        @{ File = 'CMakeLists.txt'; Pattern = '^(project\(fairyfly VERSION )[0-9.]+( )'; Value = $plain },
        @{ File = 'vcpkg.json'; Pattern = '^(\s*"version"\s*:\s*")[0-9.]+(")'; Value = $plain })) {
    $path = Join-Path $Root $e.File
    $text = [IO.File]::ReadAllText($path)
    $hits = [regex]::Matches($text, $e.Pattern, 'Multiline').Count
    if ($hits -ne 1) { throw "$($e.File): expected one match of /$($e.Pattern)/, found $hits; nothing was changed" }
    [pscustomobject]@{ Path = $path; File = $e.File; Text = [regex]::Replace($text, $e.Pattern, "`${1}$($e.Value)`${2}", 'Multiline'); Value = $e.Value }
}
foreach ($e in $edits) {
    [IO.File]::WriteAllText($e.Path, $e.Text, [Text.UTF8Encoding]::new($false))
    Write-Host "$($e.File): $($e.Value)"
}
