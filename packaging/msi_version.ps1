<#
.SYNOPSIS
  Prints the MSI ProductVersion for fairyfly's calendar version YYYY.MM.DD[.N]: (YYYY-2000).MM.(DD*100+N), e.g.
  2026.10.24 -> 26.10.2400, a further release on the same day 2026.10.24.1 -> 26.10.2401 (N = 1..99).
.DESCRIPTION
  Windows Installer compares only the first three fields of a ProductVersion (major < 256, minor < 256,
  build < 65536), so N goes into the third one: 26.10.2401 replaces 26.10.2400, and 26.10.2400 is refused over
  26.10.2401 as a downgrade (MajorUpgrade in packaging/fairyfly.wxs). Add/Remove Programs shows this string, which is
  also the winget DisplayVersion (New-WingetManifest.ps1). Up to 2026.10.24.1 the MSI was (YYYY-2000).MM.DD[.N]
  (26.10.24.1, where Windows Installer ignored N); every version of this scheme is above those.
.PARAMETER Version
  Calendar version or tag (default: FAIRYFLY_VERSION from src/include/version.h).
#>
param([string]$Version = '')
$ErrorActionPreference = 'Stop'
if (-not $Version) {
    $header = Get-Content (Join-Path $PSScriptRoot '..\src\include\version.h') -Raw
    if ($header -notmatch 'FAIRYFLY_VERSION\s*=\s*"([0-9.]+)"') { throw 'FAIRYFLY_VERSION not found in version.h' }
    $Version = $Matches[1]
}
if ($Version -cnotmatch '^v?(\d{4})\.(\d{1,2})\.(\d{1,2})(?:\.([1-9]\d{0,4}))?$') { throw "not a calendar version: $Version" }
$y = [int]$Matches[1]; $m = [int]$Matches[2]; $d = [int]$Matches[3]; $n = $Matches[4]
if ($y - 2000 -lt 0 -or $y - 2000 -gt 255) { throw "year out of the MSI range: $Version" }
if ($m -lt 1 -or $m -gt 12 -or $d -lt 1 -or $d -gt [datetime]::DaysInMonth($y, $m)) { throw "not a date: $Version" }
if ($n -and [int]$n -gt 99) { throw "same-day release number out of the MSI range (DD*100+N, N 1..99): $Version" }
"{0}.{1}.{2}" -f ($y - 2000), $m, ($d * 100 + [int]$n)
