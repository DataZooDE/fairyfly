<#
.SYNOPSIS
  Stages the MSIX layout folder that `MakeAppx pack /d <Layout>` packs: the MSI's payload, AppxManifest.xml and the
  logo images the manifest references. Prints the staged files (relative paths). Any pwsh 7, Windows or Linux.
.DESCRIPTION
  The payload is not listed here: it is the <File> elements of packaging/fairyfly.wxs (by their installed Name, taken
  from -BinDir), so the MSIX ships exactly what the MSI ships. Refused (an exception, nothing half-staged is left
  behind as success):
  - a file the MSI ships that -BinDir does not have,
  - an Executable of the manifest that the MSI does not ship (MSI and MSIX have diverged),
  - an Assets\ image the manifest references that -AssetsDir does not have.
  Checked by packaging/msix/tests/Test-MsixLayout.ps1; run in CI by build.yml (build-windows-msix) before MakeAppx.
.PARAMETER BinDir
  Build output with the MSI's files, e.g. build-msix/Release.
.PARAMETER Manifest
  The rendered manifest, e.g. build-msix/msix/AppxManifest.xml (CMake, FAIRYFLY_PACKAGE=msix).
.PARAMETER Layout
  The folder to stage into; it is emptied first.
#>
param(
    [Parameter(Mandatory)][string]$BinDir,
    [Parameter(Mandatory)][string]$Manifest,
    [Parameter(Mandatory)][string]$Layout,
    [string]$Wxs = (Join-Path (Split-Path $PSScriptRoot -Parent) 'fairyfly.wxs'),
    [string]$AssetsDir = (Join-Path $PSScriptRoot 'Assets')
)
$ErrorActionPreference = 'Stop'

# the MSI payload: installed file names of packaging/fairyfly.wxs
$wx = [xml](Get-Content $Wxs -Raw)
$wns = New-Object Xml.XmlNamespaceManager($wx.NameTable)
$wns.AddNamespace('w', 'http://wixtoolset.org/schemas/v4/wxs')
$payload = @($wx.SelectNodes('//w:File', $wns) | ForEach-Object { $_.GetAttribute('Name') })
if (-not $payload) { throw "${Wxs}: no <File> elements, nothing to package" }
foreach ($f in $payload) {
    if (-not $f -or $f -match '[\\/]') { throw "${Wxs}: <File> Name '$f' must be a plain file name" }
}

# what the manifest needs: its executables (all in the MSI payload) and its Assets\ images
$mx = [xml](Get-Content $Manifest -Raw)
$exes = @($mx.SelectNodes('//@Executable') | ForEach-Object { $_.Value } | Sort-Object -Unique)
foreach ($e in $exes) {
    if ($payload -notcontains $e) {
        throw "the manifest starts $e, which the MSI does not ship (packaging/fairyfly.wxs: $($payload -join ', ')): MSI and MSIX have diverged"
    }
}
$images = @($mx.SelectNodes('//@*') | Where-Object { $_.Value -match '^Assets\\' } | ForEach-Object { $_.Value })
$images += @($mx.SelectNodes('//*[starts-with(text(), "Assets\")]') | ForEach-Object { $_.InnerText })
$images = @($images | ForEach-Object { $_.Substring(7) } | Sort-Object -Unique)

$missing = @($payload | Where-Object { -not (Test-Path -LiteralPath (Join-Path $BinDir $_) -PathType Leaf) })
if ($missing) { throw "the MSI ships $($missing -join ', '), not found in $BinDir" }
$missing = @($images | Where-Object { -not (Test-Path -LiteralPath (Join-Path $AssetsDir $_) -PathType Leaf) })
if ($missing) { throw "the manifest references Assets\$($missing -join ', Assets\'), not found in $AssetsDir" }

if (Test-Path -LiteralPath $Layout) { Remove-Item -LiteralPath $Layout -Recurse -Force }
New-Item -ItemType Directory -Force (Join-Path $Layout 'Assets') | Out-Null
foreach ($f in $payload) {
    Copy-Item -LiteralPath (Join-Path $BinDir $f) -Destination (Join-Path $Layout $f)
    $f
}
Copy-Item -LiteralPath $Manifest -Destination (Join-Path $Layout 'AppxManifest.xml')
'AppxManifest.xml'
foreach ($i in $images) {
    Copy-Item -LiteralPath (Join-Path $AssetsDir $i) -Destination (Join-Path (Join-Path $Layout 'Assets') $i)
    "Assets/$i"
}
