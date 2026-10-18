<#
.SYNOPSIS
  Prints the MSI ProductVersion for fairyfly's calendar version: YYYY.MM.DD -> (YYYY-2000).MM.DD (MSI major < 256).
.PARAMETER Version
  Calendar version (default: FAIRYFLY_VERSION from src/include/version.h).
#>
param([string]$Version = '')
$ErrorActionPreference = 'Stop'
if (-not $Version) {
    $header = Get-Content (Join-Path $PSScriptRoot '..\src\include\version.h') -Raw
    if ($header -notmatch 'FAIRYFLY_VERSION\s*=\s*"([0-9.]+)"') { throw 'FAIRYFLY_VERSION not found in version.h' }
    $Version = $Matches[1]
}
if ($Version -notmatch '^v?(\d{4})\.(\d{1,2})\.(\d{1,2})$') { throw "not a calendar version: $Version" }
$y = [int]$Matches[1] - 2000
if ($y -lt 0 -or $y -gt 255) { throw "year out of the MSI range: $Version" }
"{0}.{1}.{2}" -f $y, [int]$Matches[2], [int]$Matches[3]
