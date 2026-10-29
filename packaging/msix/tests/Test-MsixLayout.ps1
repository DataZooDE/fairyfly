<#
.SYNOPSIS
  Checks packaging/msix/New-MsixLayout.ps1, which stages the MSIX layout folder (what `MakeAppx pack /d` packs), and the
  logo assets in packaging/msix/Assets. Any pwsh 7 with cmake on PATH, Windows or Linux (CI: build.yml, job
  msix-manifest; the real pack runs in build-windows-msix).
.DESCRIPTION
  The payload of the MSIX is the payload of the MSI: the <File> elements of packaging/fairyfly.wxs, nothing more and
  nothing less. The layout must hold exactly those files (taken from the build output by their installed names),
  AppxManifest.xml and the Assets\ images the manifest references. Staging must fail when the MSI lists a file the
  build did not produce, when the manifest starts an executable the MSI does not ship (the two have diverged), and
  when the manifest references an image packaging/msix/Assets does not have. Every image in packaging/msix/Assets is
  referenced by the manifest and has the pixel size its name says (Store: Square44x44Logo 44x44, Square150x150Logo
  150x150, Wide310x150Logo 310x150, StoreLogo 50x50).
#>
$ErrorActionPreference = 'Stop'
$repo = (Resolve-Path (Join-Path $PSScriptRoot (Join-Path '..' (Join-Path '..' '..')))).Path
$template = Join-Path $repo (Join-Path 'packaging' (Join-Path 'msix' 'AppxManifest.xml.in'))
$cmakeScript = Join-Path $repo (Join-Path 'cmake' 'msix_manifest.cmake')
$stage = Join-Path $repo (Join-Path 'packaging' (Join-Path 'msix' 'New-MsixLayout.ps1'))
$wxs = Join-Path $repo (Join-Path 'packaging' 'fairyfly.wxs')
$assets = Join-Path $repo (Join-Path 'packaging' (Join-Path 'msix' 'Assets'))
$failures = 0
function Check([bool]$cond, [string]$msg) {
    if ($cond) { Write-Host "PASS: $msg" } else { Write-Host "FAIL: $msg"; $script:failures++ }
}

$tmp = Join-Path ([IO.Path]::GetTempPath()) ("ffy-msix-layout-" + [guid]::NewGuid())
New-Item -ItemType Directory -Force $tmp | Out-Null

# Width and height of a PNG from its IHDR chunk (bytes 16..23, big-endian), $null for anything that is no PNG.
function PngSize([string]$path) {
    $b = [IO.File]::ReadAllBytes($path)
    if ($b.Length -lt 24 -or $b[0] -ne 0x89 -or $b[1] -ne 0x50 -or $b[2] -ne 0x4E -or $b[3] -ne 0x47) { return $null }
    $w = ([int]$b[16] -shl 24) -bor ([int]$b[17] -shl 16) -bor ([int]$b[18] -shl 8) -bor [int]$b[19]
    $h = ([int]$b[20] -shl 24) -bor ([int]$b[21] -shl 16) -bor ([int]$b[22] -shl 8) -bor [int]$b[23]
    "${w}x${h}"
}

# Runs the staging script; Ok and the error message (it throws on every refusal).
function Stage([string]$bin, [string]$manifest, [string]$layout, [string]$wxsFile = $wxs, [string]$assetsDir = $assets) {
    try {
        $files = & $stage -BinDir $bin -Manifest $manifest -Layout $layout -Wxs $wxsFile -AssetsDir $assetsDir
        [pscustomobject]@{ Ok = $true; Files = @($files); Error = '' }
    } catch {
        [pscustomobject]@{ Ok = $false; Files = @(); Error = $_.Exception.Message }
    }
}

function LayoutFiles([string]$layout) {
    if (-not (Test-Path $layout)) { return @() }
    @(Get-ChildItem $layout -Recurse -File | ForEach-Object {
        [IO.Path]::GetRelativePath($layout, $_.FullName).Replace('\', '/')
    } | Sort-Object)
}

try {
    Check (Test-Path $stage) 'packaging/msix/New-MsixLayout.ps1 exists'

    # the manifest as CMake renders it (defaults: the Partner Center identity)
    $manifest = Join-Path $tmp 'AppxManifest.xml'
    $log = & cmake '-DVERSION=2026.10.24.1' "-DTEMPLATE=$template" "-DOUT=$manifest" -P $cmakeScript 2>&1 | Out-String
    Check ($LASTEXITCODE -eq 0) "the manifest renders$(if ($LASTEXITCODE) { ": $log" })"

    # the MSI payload, as the wxs lists it
    $wx = [xml](Get-Content $wxs -Raw)
    $wns = New-Object Xml.XmlNamespaceManager($wx.NameTable)
    $wns.AddNamespace('w', 'http://wixtoolset.org/schemas/v4/wxs')
    $msiFiles = @($wx.SelectNodes('//w:File', $wns) | ForEach-Object { $_.Name } | Sort-Object)
    Write-Host "MSI payload (fairyfly.wxs): $($msiFiles -join ', ')"
    Check ($msiFiles -contains 'fairyfly.exe' -and $msiFiles -contains 'fairyfly-tray.exe') 'the MSI ships fairyfly.exe and fairyfly-tray.exe'

    # a build output folder: the MSI's files plus build by-products that must not be packaged
    $bin = Join-Path $tmp 'Release'
    New-Item -ItemType Directory -Force $bin | Out-Null
    foreach ($f in $msiFiles + 'unit_tests.exe', 'fairyfly.pdb', 'fairyfly.lib') {
        Set-Content -Path (Join-Path $bin $f) -Value "dummy $f" -NoNewline
    }

    Write-Host "`n=== layout from the real wxs, manifest and assets ==="
    $layout = Join-Path $tmp 'layout'
    $r = Stage $bin $manifest $layout
    Check $r.Ok "staging succeeds$(if (-not $r.Ok) { ": $($r.Error)" })"
    $expected = @(@('AppxManifest.xml') + $msiFiles + @('Assets/Square44x44Logo.png', 'Assets/Square150x150Logo.png',
                  'Assets/StoreLogo.png', 'Assets/Wide310x150Logo.png') | Sort-Object)
    $got = LayoutFiles $layout
    Check (($got -join '|') -eq ($expected -join '|')) "the layout holds exactly the MSI payload, AppxManifest.xml and the referenced assets (got: $($got -join ', '))"
    Check ((Get-Content (Join-Path $layout 'fairyfly.exe') -Raw -ErrorAction SilentlyContinue) -eq 'dummy fairyfly.exe') 'fairyfly.exe is copied from the build output'
    Check ((Get-Content (Join-Path $layout 'AppxManifest.xml') -Raw -ErrorAction SilentlyContinue) -eq (Get-Content $manifest -Raw)) 'AppxManifest.xml is the rendered manifest'
    # staging again replaces the folder (no stale file from an earlier run)
    Set-Content -Path (Join-Path $layout 'stale.txt') -Value 'x' -ErrorAction SilentlyContinue
    $r = Stage $bin $manifest $layout
    Check ($r.Ok -and -not (Test-Path (Join-Path $layout 'stale.txt'))) 'a second staging starts from an empty layout'

    Write-Host "`n=== manifest executables are in the MSI payload ==="
    $mx = [xml](Get-Content $manifest -Raw)
    $exes = @($mx.SelectNodes('//@Executable') | ForEach-Object { $_.Value } | Sort-Object -Unique)
    foreach ($e in $exes) { Check ($msiFiles -contains $e) "manifest executable $e is shipped by the MSI" }

    Write-Host "`n=== divergence and missing files are refused ==="
    $wxsExtra = Join-Path $tmp 'extra.wxs'
    $wxsText = Get-Content $wxs -Raw
    $extraFile = '<File Id="ExtraDll" Source="extra.dll" Name="fairyfly-extra.dll" KeyPath="no" />'
    Set-Content -Path $wxsExtra -Value ($wxsText -replace '(<File Id="FairyflyTrayExeFile"[^>]*/>)', "`$1`n        $extraFile") -NoNewline
    $r = Stage $bin $manifest (Join-Path $tmp 'l2') $wxsExtra
    Check (-not $r.Ok -and $r.Error -match 'fairyfly-extra\.dll') "a file the MSI ships but the build lacks is refused ($($r.Error))"
    Set-Content -Path (Join-Path $bin 'fairyfly-extra.dll') -Value 'dll' -NoNewline
    $r = Stage $bin $manifest (Join-Path $tmp 'l3') $wxsExtra
    Check ($r.Ok -and (Test-Path (Join-Path $tmp (Join-Path 'l3' 'fairyfly-extra.dll')))) 'a file added to the MSI is packaged in the MSIX too'
    Remove-Item (Join-Path $bin 'fairyfly-extra.dll')

    $manifestOther = Join-Path $tmp 'other.xml'
    Set-Content -Path $manifestOther -Value ((Get-Content $manifest -Raw) -replace 'Executable="fairyfly-tray.exe"', 'Executable="fairyfly-launcher.exe"') -NoNewline
    Set-Content -Path (Join-Path $bin 'fairyfly-launcher.exe') -Value 'x' -NoNewline
    $r = Stage $bin $manifestOther (Join-Path $tmp 'l4')
    Check (-not $r.Ok -and $r.Error -match 'fairyfly-launcher\.exe') "a manifest executable the MSI does not ship is refused ($($r.Error))"

    $binMissing = Join-Path $tmp 'Release-missing'
    New-Item -ItemType Directory -Force $binMissing | Out-Null
    Set-Content -Path (Join-Path $binMissing 'fairyfly.exe') -Value 'x' -NoNewline
    $r = Stage $binMissing $manifest (Join-Path $tmp 'l5')
    Check (-not $r.Ok -and $r.Error -match 'fairyfly-tray\.exe') "a missing build output is refused ($($r.Error))"

    $assetsPartial = Join-Path $tmp 'assets-partial'
    New-Item -ItemType Directory -Force $assetsPartial | Out-Null
    if (Test-Path $assets) { Copy-Item (Join-Path $assets '*') $assetsPartial }
    Remove-Item (Join-Path $assetsPartial 'StoreLogo.png') -ErrorAction SilentlyContinue
    $r = Stage $bin $manifest (Join-Path $tmp 'l6') $wxs $assetsPartial
    Check (-not $r.Ok -and $r.Error -match 'StoreLogo\.png') "a referenced image that is missing is refused ($($r.Error))"

    Write-Host "`n=== the logo assets ==="
    $sizes = @{ 'Square44x44Logo.png' = '44x44'; 'Square150x150Logo.png' = '150x150'; 'Wide310x150Logo.png' = '310x150'; 'StoreLogo.png' = '50x50' }
    foreach ($k in $sizes.Keys | Sort-Object) {
        $p = Join-Path $assets $k
        $s = if (Test-Path $p) { PngSize $p } else { $null }
        Check ($s -eq $sizes[$k]) "packaging/msix/Assets/$k is a $($sizes[$k]) PNG (got: $s)"
    }
    $refs = @($mx.SelectNodes('//@*') | Where-Object { $_.Value -match '^Assets\\' } | ForEach-Object { $_.Value })
    $refs += @($mx.SelectNodes('//*[starts-with(text(), "Assets\")]') | ForEach-Object { $_.InnerText })
    $refs = @($refs | ForEach-Object { $_.Substring(7) } | Sort-Object -Unique)
    $have = @(if (Test-Path $assets) { Get-ChildItem $assets -File | ForEach-Object { $_.Name } | Sort-Object })
    Check (($refs -join '|') -eq ($have -join '|')) "packaging/msix/Assets holds exactly the images the manifest references (manifest: $($refs -join ', '); folder: $($have -join ', '))"
} finally {
    Remove-Item $tmp -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failures) { Write-Host "`n$failures check(s) FAILED"; exit 1 }
Write-Host "`nALL MSIX LAYOUT CHECKS PASSED"
exit 0
