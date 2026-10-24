<#
.SYNOPSIS
  Prints the MSI ProductVersion for fairyfly's calendar version: YYYY.MM.DD -> (YYYY-2000).MM.DD (MSI major < 256),
  and a further release on the same day YYYY.MM.DD.N -> (YYYY-2000).MM.DD.N.
.DESCRIPTION
  Windows Installer ignores the 4th field when it compares versions, so 26.10.24.1 counts as 26.10.24: the package's
  MajorUpgrade AllowSameVersionUpgrades="yes" (packaging/fairyfly.wxs) makes it replace the installed 26.10.24 (a new
  ProductCode per build), and 26.10.25 replaces either. Add/Remove Programs shows the full string (26.10.24.1).
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
if ($n -and [int]$n -gt 65535) { throw "same-day release number out of the MSI range (65535): $Version" }
$msi = "{0}.{1}.{2}" -f ($y - 2000), $m, $d
if ($n) { $msi += ".$([int]$n)" }
$msi
