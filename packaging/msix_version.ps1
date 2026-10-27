<#
.SYNOPSIS
  Prints the MSIX package version for fairyfly's calendar version YYYY.MM.DD[.N]: YYYY.MM.(DD*100+N).0, e.g.
  2026.10.24 -> 2026.10.2400.0, a further release on the same day 2026.10.24.1 -> 2026.10.2401.0 (N = 1..99).
.DESCRIPTION
  MSIX needs four numeric fields (0..65535 each) and the Microsoft Store requires the fourth to be 0, so N goes into the
  third field as for the MSI. The calendar version is checked by packaging/msi_version.ps1 (same rules), whose
  (YYYY-2000).MM.(DD*100+N) this prints with the full year and .0. cmake/msix_manifest.cmake (fairyfly_msix_version)
  computes the same for the manifest; packaging/tests/Test-VersionScripts.ps1 checks that both agree.
.PARAMETER Version
  Calendar version or tag (default: FAIRYFLY_VERSION from src/include/version.h).
#>
param([string]$Version = '')
$ErrorActionPreference = 'Stop'
$msi = (& (Join-Path $PSScriptRoot 'msi_version.ps1') -Version $Version | Out-String).Trim()
$p = $msi.Split('.')
"{0}.{1}.{2}.0" -f (2000 + [int]$p[0]), [int]$p[1], [int]$p[2]
