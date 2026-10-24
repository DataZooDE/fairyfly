<#
.SYNOPSIS
  Checks a release tag against the product version fairyfly.exe reports (release.yml, before anything is published).
.DESCRIPTION
  The tag must be vYYYY.MM.DD or vYYYY.MM.DD.N (N = 1..99, as set_version.ps1 and the MSI version
  (YYYY-2000).MM.(DD*100+N) allow) and equal "v<Version>". The updater
  (src/update/update_model.cpp, parse_release) only takes tags with a lowercase "v", so the check is case-sensitive.
  Throws on a bad tag; prints nothing otherwise.
.PARAMETER Tag
  The git tag (RELEASE_TAG), e.g. v2026.10.24.1.
.PARAMETER Version
  What fairyfly.exe --version prints, e.g. 2026.10.24.1.
#>
param(
    [Parameter(Mandatory)][AllowEmptyString()][string]$Tag,
    [Parameter(Mandatory)][string]$Version
)
$ErrorActionPreference = 'Stop'
if ($Tag -cnotmatch '^v\d{4}\.\d{2}\.\d{2}(?:\.[1-9]\d?)?$') {
    throw "Tag '$Tag' is not vYYYY.MM.DD or vYYYY.MM.DD.N (N 1..99, lowercase v)"
}
if ($Tag -cne "v$Version") {
    throw "Tag '$Tag' does not match the product version 'v$Version': bump src/include/version.h, CMakeLists.txt and vcpkg.json (packaging/set_version.ps1) before tagging"
}
