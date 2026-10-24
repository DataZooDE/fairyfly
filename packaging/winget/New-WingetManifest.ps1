<#
.SYNOPSIS
  Renders the winget manifests (schema 1.10.0) of DataZoo.fairyfly for one release from the built per-user MSI.
.DESCRIPTION
  Reads ProductCode, UpgradeCode, ProductVersion, Manufacturer and the summary-information word count of fairyfly.msi
  through the Windows Installer COM API (WindowsInstaller.Installer, read-only), computes the SHA-256 and fills the
  templates in packaging/winget/templates/ into -OutDir:
    DataZoo.fairyfly.yaml, DataZoo.fairyfly.installer.yaml, DataZoo.fairyfly.locale.en-US.yaml
  It refuses an MSI that does not match the release (version, UpgradeCode, manufacturer) or that is not a per-user
  package without elevation. Windows only (COM). Returns an object with the values it used.
.PARAMETER Version
  Release tag or calendar version (v2026.10.05 or 2026.10.05; a further release on the same day v2026.10.05.1).
  PackageVersion is the calendar version (2026.10.05.1, which winget orders after 2026.10.05 and before 2026.10.06);
  AppsAndFeaturesEntries DisplayVersion is the MSI ProductVersion (26.10.501, packaging/msi_version.ps1), which is
  what Windows shows in Add/Remove Programs (asserted by tests/Test-WingetUserInstall.ps1).
.PARAMETER MsiPath
  The fairyfly.msi that is (or will be) the release asset.
.PARAMETER OutDir
  Directory for the three manifest files (created; existing manifest files are overwritten).
.PARAMETER InstallerUrl
  Download URL of the MSI. Default: the GitHub release asset
  https://github.com/DataZooDE/fairyfly/releases/download/<tag>/fairyfly.msi
.EXAMPLE
  ./packaging/winget/New-WingetManifest.ps1 -Version v2026.10.05 -MsiPath fairyfly.msi -OutDir winget
#>
param(
    [Parameter(Mandatory)][string]$Version,
    [Parameter(Mandatory)][string]$MsiPath,
    [Parameter(Mandatory)][string]$OutDir,
    [string]$InstallerUrl = '',
    [string]$Repository = 'DataZooDE/fairyfly'
)
$ErrorActionPreference = 'Stop'
$UpgradeCode = '{AEC9807E-BC74-4802-8816-25F3AB39BA29}'   # packaging/fairyfly.wxs, never changes
$Manufacturer = 'DataZoo GmbH'

if ($Version -cnotmatch '^v?(\d{4})\.(\d{2})\.(\d{2})(\.[1-9]\d?)?$') { throw "not a calendar version or tag (vYYYY.MM.DD or vYYYY.MM.DD.N, N 1..99): $Version" }
$packageVersion = "$($Matches[1]).$($Matches[2]).$($Matches[3])$($Matches[4])"
$releaseDate = "$($Matches[1])-$($Matches[2])-$($Matches[3])"
$tag = "v$packageVersion"
$msiVersion = (& (Join-Path $PSScriptRoot '..\msi_version.ps1') -Version $packageVersion).Trim()
if (-not $InstallerUrl) { $InstallerUrl = "https://github.com/$Repository/releases/download/$tag/fairyfly.msi" }
$MsiPath = (Resolve-Path $MsiPath).Path

function Invoke-Com($obj, [string]$member, [Reflection.BindingFlags]$kind, [object[]]$argList) {
    $obj.GetType().InvokeMember($member, $kind, $null, $obj, $argList)
}
function Read-MsiInfo([string]$path) {
    $installer = New-Object -ComObject WindowsInstaller.Installer
    $db = Invoke-Com $installer 'OpenDatabase' InvokeMethod @($path, 0)   # 0 = msiOpenDatabaseModeReadOnly
    try {
        $props = @{}
        foreach ($name in 'ProductCode', 'UpgradeCode', 'ProductVersion', 'ProductName', 'Manufacturer', 'ALLUSERS', 'MSIINSTALLPERUSER') {
            $view = Invoke-Com $db 'OpenView' InvokeMethod @("SELECT ``Value`` FROM ``Property`` WHERE ``Property`` = '$name'")
            Invoke-Com $view 'Execute' InvokeMethod $null | Out-Null
            $record = Invoke-Com $view 'Fetch' InvokeMethod $null
            $props[$name] = if ($record) { [string](Invoke-Com $record 'StringData' GetProperty @(1)) } else { '' }
            Invoke-Com $view 'Close' InvokeMethod $null | Out-Null
            [void][Runtime.InteropServices.Marshal]::ReleaseComObject($view)
        }
        $summary = Invoke-Com $db 'SummaryInformation' GetProperty @(0)
        $props['WordCount'] = [int](Invoke-Com $summary 'Property' GetProperty @(15))   # PID_WORDCOUNT
        [void][Runtime.InteropServices.Marshal]::ReleaseComObject($summary)
        return $props
    } finally {
        [void][Runtime.InteropServices.Marshal]::ReleaseComObject($db)
        [void][Runtime.InteropServices.Marshal]::ReleaseComObject($installer)
        [GC]::Collect(); [GC]::WaitForPendingFinalizers()
    }
}

$msi = Read-MsiInfo $MsiPath
if ($msi.ProductCode -notmatch '^\{[0-9A-F]{8}(-[0-9A-F]{4}){3}-[0-9A-F]{12}\}$') { throw "unexpected ProductCode '$($msi.ProductCode)'" }
if ($msi.UpgradeCode -ne $UpgradeCode) { throw "UpgradeCode is '$($msi.UpgradeCode)', expected $UpgradeCode" }
if ($msi.ProductVersion -ne $msiVersion) { throw "MSI ProductVersion is '$($msi.ProductVersion)', expected $msiVersion for $tag" }
if ($msi.Manufacturer -ne $Manufacturer) { throw "MSI Manufacturer is '$($msi.Manufacturer)'" }
if ($msi.ProductName -ne 'fairyfly') { throw "MSI ProductName is '$($msi.ProductName)'" }
# per-user package: ALLUSERS unset, and word count bit 3 (8) = "elevated privileges are not required" (no UAC prompt)
if ($msi.ALLUSERS) { throw "MSI sets ALLUSERS=$($msi.ALLUSERS): not a per-user package" }
if (-not ($msi.WordCount -band 8)) { throw "MSI summary word count $($msi.WordCount) lacks bit 8: the package asks for elevation" }
$sha256 = (Get-FileHash $MsiPath -Algorithm SHA256).Hash

$values = [ordered]@{
    PackageVersion  = $packageVersion
    Tag             = $tag
    ReleaseDate     = $releaseDate
    MsiVersion      = $msiVersion
    ProductCode     = $msi.ProductCode
    UpgradeCode     = $msi.UpgradeCode
    InstallerUrl    = $InstallerUrl
    InstallerSha256 = $sha256
}
New-Item -ItemType Directory -Force $OutDir | Out-Null
foreach ($template in Get-ChildItem (Join-Path $PSScriptRoot 'templates') -Filter '*.yaml') {
    $text = Get-Content $template.FullName -Raw
    foreach ($k in $values.Keys) { $text = $text.Replace("{{$k}}", [string]$values[$k]) }
    if ($text -match '\{\{(\w+)\}\}') { throw "$($template.Name): no value for {{$($Matches[1])}}" }
    # UTF-8 without BOM, LF line endings (winget-pkgs style)
    [IO.File]::WriteAllText((Join-Path $OutDir $template.Name), ($text -replace "`r`n", "`n"), [Text.UTF8Encoding]::new($false))
}
Write-Host "winget manifests for DataZoo.fairyfly $packageVersion in $OutDir (MSI $msiVersion, $($msi.ProductCode), sha256 $sha256)"
[pscustomobject]$values
