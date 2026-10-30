<#
.SYNOPSIS
  Checks the version of the packed MSIX against the release tag and the earlier releases (release.yml, job build-msix,
  before the package is uploaded; build.yml runs the same check as a release dry run).
.DESCRIPTION
  - The tag must be vYYYY.MM.DD or vYYYY.MM.DD.N (N = 1..99, lowercase v, as check_release_tag.ps1).
  - The package version (AppxManifest.xml Package/Identity/Version of the packed MSIX) must be exactly
    msix_version.ps1 of the tag: YYYY.MM.(DD*100+N).0, e.g. v2026.10.24.1 -> 2026.10.2401.0.
  - It must be strictly above the MSIX version of every earlier release: Partner Center refuses a package whose version
    is not higher than the last one submitted. The earlier MSIX versions are computed from the release tags with the
    same mapping (releases before the MSIX existed shipped none, but the order is the one the Store will see). The tag
    itself is left out (a re-run of a release), as are tags that are no calendar version.
  Throws on a mismatch; prints one line otherwise. Checked by packaging/tests/Test-VersionScripts.ps1.
.PARAMETER Tag
  The release tag (RELEASE_TAG), e.g. v2026.10.24.1.
.PARAMETER MsixVersion
  Package/Identity/Version of the packed MSIX.
.PARAMETER PreviousTags
  The tags of the published releases (gh api repos/<repo>/releases), in any order; may be empty.
#>
param(
    [Parameter(Mandatory)][AllowEmptyString()][string]$Tag,
    [Parameter(Mandatory)][AllowEmptyString()][string]$MsixVersion,
    [AllowEmptyCollection()][AllowEmptyString()][string[]]$PreviousTags = @()
)
$ErrorActionPreference = 'Stop'
$toMsix = Join-Path $PSScriptRoot 'msix_version.ps1'

if ($Tag -cnotmatch '^v\d{4}\.\d{2}\.\d{2}(?:\.[1-9]\d?)?$') {
    throw "Tag '$Tag' is not vYYYY.MM.DD or vYYYY.MM.DD.N (N 1..99, lowercase v)"
}
$expected = (& $toMsix -Version $Tag | Out-String).Trim()
if ($MsixVersion -cne $expected) {
    throw "the MSIX package version '$MsixVersion' is not the one of the tag ${Tag}: expected $expected (packaging/msix_version.ps1)"
}

$newest = $null
foreach ($t in $PreviousTags) {
    if (-not $t -or $t -ceq $Tag) { continue }
    try { $v = [version](& $toMsix -Version $t | Out-String).Trim() }
    catch { Write-Host "ignoring the release tag '$t' (no calendar version)"; continue }
    if (-not $newest -or $v -gt $newest.Version) { $newest = [pscustomobject]@{ Tag = $t; Version = $v } }
}
if ($newest -and [version]$expected -le $newest.Version) {
    throw "the MSIX version $expected of $Tag is not above $($newest.Version) of the earlier release $($newest.Tag): the Microsoft Store requires a higher package version for every submission"
}
$prev = if ($newest) { "above $($newest.Version) of $($newest.Tag)" } else { 'no earlier release' }
"MSIX version $expected for $Tag OK ($prev)"
