<#
.SYNOPSIS
  Checks the winget manifest templates (packaging/winget/templates) against the rules of the community repository
  microsoft/winget-pkgs (CI: .github/workflows/winget.yml; any pwsh 7, no install, no Windows needed).
.DESCRIPTION
  winget-pkgs validates every pull request (doc/Validation.md, doc/ValidationFailureGuide.md, doc/Policies.md in
  https://github.com/microsoft/winget-pkgs). What a template can get wrong is checked here, before a release renders it:
  - schema: ManifestVersion and the "# yaml-language-server: $schema=" header of each file name the version the
    repository recommends (1.12.0; Manifest-Version-Deprecated otherwise), the header is the first line;
  - defaultLocale: the required fields, ShortDescription 3..256 characters, at most 16 tags of at most 40 characters
    (lower case, hyphens, no spaces), Moniker likewise, https URLs only, PrivacyUrl present (fairyfly can relay SAP
    screens through the cloud link: Windows Package Manager policy 1.5.1), no Agreements (only verified developers);
  - consistency: Publisher and PackageName equal the MSI's Manufacturer and Name (packaging/fairyfly.wxs, so winget
    correlates the Add/Remove Programs entry), Copyright equals the copyright of the exe (src/version_info.rc.in).
  The rendered values (versions, codes, hash) are checked by Test-WingetUserInstall.ps1 with `winget validate`.
#>
$ErrorActionPreference = 'Stop'
$winget = Split-Path $PSScriptRoot -Parent
$repo = Split-Path (Split-Path $winget -Parent) -Parent
$templates = Join-Path $winget 'templates'
$schemaVersion = '1.12.0'
$failures = 0
function Check([bool]$cond, [string]$msg) {
    if ($cond) { Write-Host "PASS: $msg" } else { Write-Host "FAIL: $msg"; $script:failures++ }
}
# top-level scalar "Key: value" of a template (quotes removed); $null when absent
function Get-Field([string]$text, [string]$key) {
    $m = [regex]::Match($text, "(?m)^$([regex]::Escape($key)):[ \t]*(.*?)[ \t]*$")
    if (-not $m.Success) { return $null }
    return $m.Groups[1].Value.Trim("'", '"')
}
# top-level list "Key:\n- a\n- b"
function Get-List([string]$text, [string]$key) {
    $m = [regex]::Match($text, "(?m)^$([regex]::Escape($key)):[ \t]*\r?\n((?:- .*\r?\n?)+)")
    if (-not $m.Success) { return @() }
    return @($m.Groups[1].Value -split '\r?\n' | Where-Object { $_ } | ForEach-Object { $_.Substring(2).Trim() })
}
# block scalar "Key: |-" followed by indented lines
function Get-Block([string]$text, [string]$key) {
    $m = [regex]::Match($text, "(?m)^$([regex]::Escape($key)):[ \t]*\|-?[ \t]*\r?\n((?:[ \t]+.*\r?\n?)+)")
    if (-not $m.Success) { return $null }
    return (($m.Groups[1].Value -split '\r?\n' | ForEach-Object { $_ -replace '^  ', '' }) -join "`n").Trim()
}

$files = [ordered]@{
    'DataZoo.fairyfly.yaml'                = 'version'
    'DataZoo.fairyfly.installer.yaml'      = 'installer'
    'DataZoo.fairyfly.locale.en-US.yaml'   = 'defaultLocale'
}
$text = @{}
Write-Host "=== schema $schemaVersion ==="
foreach ($name in $files.Keys) {
    $path = Join-Path $templates $name
    Check (Test-Path $path) "$name exists"
    if (-not (Test-Path $path)) { continue }
    $t = Get-Content $path -Raw
    $text[$name] = $t
    $type = $files[$name]
    $first = ($t -split '\r?\n')[0]
    Check ($first -ceq "# yaml-language-server: `$schema=https://aka.ms/winget-manifest.$type.$schemaVersion.schema.json") "$name first line is the $type $schemaVersion schema header (is '$first')"
    Check ((Get-Field $t 'ManifestVersion') -ceq $schemaVersion) "$name ManifestVersion is $schemaVersion (is '$(Get-Field $t 'ManifestVersion')')"
    Check ((Get-Field $t 'ManifestType') -ceq $type) "$name ManifestType is $type"
    Check ((Get-Field $t 'PackageIdentifier') -ceq 'DataZoo.fairyfly') "$name PackageIdentifier is DataZoo.fairyfly"
    Check ($t -notmatch "`t") "$name has no tab characters"
}

Write-Host '=== defaultLocale ==='
$l = $text['DataZoo.fairyfly.locale.en-US.yaml']
if ($l) {
    foreach ($k in 'PackageLocale', 'Publisher', 'PackageName', 'License', 'ShortDescription') {
        Check ([bool](Get-Field $l $k)) "required field $k is set"
    }
    $short = Get-Field $l 'ShortDescription'
    Check ($short.Length -ge 3 -and $short.Length -le 256) "ShortDescription has 3..256 characters ($($short.Length))"
    $desc = Get-Block $l 'Description'
    Check ($desc -and $desc.Length -le 10000) "Description is set and at most 10000 characters"
    $tags = Get-List $l 'Tags'
    Check ($tags.Count -ge 1 -and $tags.Count -le 16) "1..16 tags ($($tags.Count))"
    Check (($tags | Select-Object -Unique).Count -eq $tags.Count) 'tags are unique'
    foreach ($tag in $tags) { Check ($tag -cmatch '^[a-z0-9][a-z0-9-]{0,39}$') "tag '$tag' is lower case with hyphens, at most 40 characters" }
    $moniker = Get-Field $l 'Moniker'
    Check ($moniker -cmatch '^[a-z0-9][a-z0-9-]{0,39}$') "Moniker '$moniker' is lower case, at most 40 characters"
    $urls = [regex]::Matches($l, '(?m)^\s*-?\s*(\w+Url):[ \t]*(\S+)')
    Check ($urls.Count -ge 5) "URL fields present ($($urls.Count))"
    foreach ($u in $urls) { Check ($u.Groups[2].Value -cmatch '^https://') "$($u.Groups[1].Value) uses https ($($u.Groups[2].Value))" }
    foreach ($k in 'PublisherUrl', 'PublisherSupportUrl', 'PrivacyUrl', 'PackageUrl', 'LicenseUrl', 'ReleaseNotesUrl') {
        Check ([bool](Get-Field $l $k)) "$k is set"
    }
    Check ($l -notmatch '(?m)^Agreements:') 'no Agreements (community repository: verified developers only)'

    Write-Host '=== consistency with the MSI and the exe ==='
    $wxs = Get-Content (Join-Path $repo 'packaging/fairyfly.wxs') -Raw
    $pkg = [regex]::Match($wxs, '<Package\s+Name="([^"]+)"\s+Manufacturer="([^"]+)"')
    Check ($pkg.Success) 'fairyfly.wxs has <Package Name=... Manufacturer=...>'
    Check ((Get-Field $l 'Publisher') -ceq $pkg.Groups[2].Value) "Publisher '$(Get-Field $l 'Publisher')' equals the MSI Manufacturer '$($pkg.Groups[2].Value)'"
    Check ((Get-Field $l 'PackageName') -ceq $pkg.Groups[1].Value) "PackageName equals the MSI Name '$($pkg.Groups[1].Value)'"
    $rc = Get-Content (Join-Path $repo 'src/version_info.rc.in') -Raw
    $copyright = [regex]::Match($rc, 'VALUE "LegalCopyright",\s*"(Copyright[^".]*)').Groups[1].Value.Trim()
    Check ($copyright -and (Get-Field $l 'Copyright') -ceq $copyright) "Copyright '$(Get-Field $l 'Copyright')' equals the exe's '$copyright'"
}

Write-Host '=== installer ==='
$i = $text['DataZoo.fairyfly.installer.yaml']
if ($i) {
    Check ((Get-Field $i 'InstallerType') -ceq 'wix') 'InstallerType wix (winget passes the MSI silent switches)'
    Check ((Get-Field $i 'Scope') -ceq 'user') 'Scope user (per-user MSI)'
    Check ($i -cmatch '(?m)^\s*-?\s*InstallerUrl: \{\{InstallerUrl\}\}') 'InstallerUrl comes from the generator'
    Check ($i -cmatch '(?m)^\s*DisplayVersion: \{\{MsiVersion\}\}') 'AppsAndFeaturesEntries DisplayVersion is the MSI ProductVersion'
}

if ($failures) { Write-Host "`n$failures check(s) failed"; exit 1 }
Write-Host "`nall template checks passed"
