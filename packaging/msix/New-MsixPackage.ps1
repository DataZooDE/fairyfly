<#
.SYNOPSIS
  Packs the UNSIGNED fairyfly-<msixversion>.msix from an msix build (FAIRYFLY_PACKAGE=msix): the same steps in CI
  (build.yml, build-windows-msix) and in the release (release.yml, build-msix). Windows, with the MSVC environment
  (dumpbin) and the Windows SDK (MakeAppx). Prints an object with Path, Version (Package/Identity/Version) and SdkBin.
.DESCRIPTION
  1. Neither exe may import a runtime DLL that neither installer carries (dumpbin /dependents).
  2. The layout is the MSI's payload, AppxManifest.xml and the logo images (New-MsixLayout.ps1).
  3. MakeAppx pack (validates the manifest against the schema) into <OutDir>\fairyfly-<msixversion>.msix.
  4. Unpack round trip: the package holds exactly the layout (plus AppxBlockMap.xml and [Content_Types].xml).
  The package is not signed here: the Microsoft Store signs what is submitted (docs/MSIX.md).
.PARAMETER BinDir
  Build output with the MSI's files, e.g. build-msix/Release.
.PARAMETER Manifest
  The rendered manifest, e.g. build-msix/msix/AppxManifest.xml.
.PARAMETER WorkDir
  Folder for layout\, unpacked\ and (unless -OutDir) out\, e.g. build-msix.
.PARAMETER OutDir
  Where the .msix goes (default <WorkDir>\out).
#>
param(
    [Parameter(Mandatory)][string]$BinDir,
    [Parameter(Mandatory)][string]$Manifest,
    [Parameter(Mandatory)][string]$WorkDir,
    [string]$OutDir = ''
)
$ErrorActionPreference = 'Stop'
if (-not $OutDir) { $OutDir = Join-Path $WorkDir 'out' }

$makeappx = Get-ChildItem 'C:\Program Files (x86)\Windows Kits\10\bin\10.*\x64\makeappx.exe' -ErrorAction SilentlyContinue |
    Sort-Object { [version]$_.Directory.Parent.Name } | Select-Object -Last 1
if (-not $makeappx) { throw 'MakeAppx.exe not found in the Windows SDK' }
$sdkBin = $makeappx.DirectoryName
Write-Host "Windows SDK tools: $sdkBin"

# the MSI ships the two exes alone: they must not need a runtime DLL that neither installer carries
foreach ($exe in 'fairyfly.exe', 'fairyfly-tray.exe') {
    $deps = @(& dumpbin.exe /nologo /dependents (Join-Path $BinDir $exe) | ForEach-Object {
        if ($_ -match '^\s+(\S+\.dll)\s*$') { $Matches[1] } })
    Write-Host "$exe imports: $($deps -join ', ')"
    $bad = @($deps | Where-Object { $_ -match '^(vcruntime|msvcp|concrt|vccorlib|ucrtbased)' -or (Test-Path (Join-Path $BinDir $_)) })
    if (-not $deps -or $bad) { throw "$exe needs $($bad -join ', '), which the MSI (packaging/fairyfly.wxs) does not ship" }
}

$version = ([xml](Get-Content $Manifest -Raw)).Package.Identity.Version
$layout = Join-Path $WorkDir 'layout'
$files = & (Join-Path $PSScriptRoot 'New-MsixLayout.ps1') -BinDir $BinDir -Manifest $Manifest -Layout $layout
Write-Host "layout: $($files -join ', ')"
New-Item -ItemType Directory -Force $OutDir | Out-Null
$msix = Join-Path $OutDir "fairyfly-$version.msix"
& "$sdkBin\makeappx.exe" pack /d $layout /p $msix /o | Out-Host
if ($LASTEXITCODE -ne 0) { throw "MakeAppx pack failed ($LASTEXITCODE)" }

# round trip: the package holds exactly the layout (plus MakeAppx's block map and content types)
$unpacked = Join-Path $WorkDir 'unpacked'
& "$sdkBin\makeappx.exe" unpack /p $msix /d $unpacked /o | Out-Host
if ($LASTEXITCODE -ne 0) { throw "MakeAppx unpack failed ($LASTEXITCODE)" }
$rel = { param($root) @(Get-ChildItem $root -Recurse -File | ForEach-Object { [IO.Path]::GetRelativePath((Resolve-Path $root).Path, $_.FullName) } | Sort-Object) }
$packed = @(& $rel $unpacked | Where-Object { $_ -notin 'AppxBlockMap.xml', '[Content_Types].xml' })
$staged = & $rel $layout
if (($packed -join '|') -ne ($staged -join '|')) { throw "package content '$($packed -join ', ')' differs from the layout '$($staged -join ', ')'" }
$id = ([xml](Get-Content (Join-Path $unpacked 'AppxManifest.xml') -Raw)).Package.Identity
if ($id.Version -ne $version) { throw "the packed manifest has version $($id.Version), the rendered one $version" }
Write-Host "packed $msix ($([math]::Round((Get-Item $msix).Length / 1MB, 2)) MB): $($id.Name) $($id.Version) $($id.Publisher)"
[pscustomobject]@{ Path = (Resolve-Path $msix).Path; Version = $version; SdkBin = $sdkBin }
