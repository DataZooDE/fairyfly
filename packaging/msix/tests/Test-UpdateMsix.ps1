$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot '../Update-Msix.Common.ps1')
$script:checks = 0
function Check($condition, $message) {
    if (-not $condition) { throw "FAIL: $message" }
    $script:checks++
}
function Refuses([scriptblock]$action, [string]$pattern) {
    try { & $action } catch { Check ($_.Exception.Message -match $pattern) $_.Exception.Message; return }
    throw "FAIL: expected refusal matching $pattern"
}
$candidate = [pscustomobject]@{Name='DataZoo.fairyfly'; Publisher='CN=05AB657A-5AC7-43BC-A7F1-92AAD620BCCC'; Version=[version]'2026.10.3000.0'; Architecture='x64'}
$installed = [pscustomobject]@{Name=$candidate.Name; Publisher=$candidate.Publisher; Version=[version]'2026.10.2800.0'}
Assert-FairyflyMsixUpdate $candidate $installed
Check $true 'newer package accepted'
Assert-FairyflyMsixUpdate $candidate $null
Check $true 'first installation accepted'
$installed.Version = $candidate.Version
Refuses { Assert-FairyflyMsixUpdate $candidate $installed } 'higher version'
Assert-FairyflyMsixUpdate $candidate $installed -SameContent $true
Check $true 'identical installed package can verify/restart without reinstall'
$installed.Version = [version]'2026.11.100.0'
Refuses { Assert-FairyflyMsixUpdate $candidate $installed } 'higher version'
$candidate.Name = 'Other.App'
Refuses { Assert-FairyflyMsixUpdate $candidate $null } 'identity'
$candidate.Name = 'DataZoo.fairyfly'
$candidate.Publisher = 'CN=Other'
Refuses { Assert-FairyflyMsixUpdate $candidate $null } 'publisher'
$candidate.Publisher = 'CN=05AB657A-5AC7-43BC-A7F1-92AAD620BCCC'
$candidate.Architecture = 'arm64'
Refuses { Assert-FairyflyMsixUpdate $candidate $null } 'architecture'
$candidate.Architecture = 'x64'
$cert = [pscustomobject]@{Subject=$candidate.Publisher; Thumbprint='AAA'}
$signature = [pscustomobject]@{Status='Valid'; SignerCertificate=$cert}
Assert-FairyflyMsixSignature $signature $candidate $null
Check $true 'trusted matching signature accepted'
$signature.Status = 'NotTrusted'
Refuses { Assert-FairyflyMsixSignature $signature $candidate $null } 'CertificatePath'
Assert-FairyflyMsixSignature $signature $candidate $cert
Check $true 'matching certificate permits trust step'
$other = [pscustomobject]@{Subject=$candidate.Publisher; Thumbprint='BBB'}
Refuses { Assert-FairyflyMsixSignature $signature $candidate $other } 'does not match'
$signature.Status = 'HashMismatch'
Refuses { Assert-FairyflyMsixSignature $signature $candidate $cert } 'signature'
$signature.Status = 'UnknownError'
$signature | Add-Member NoteProperty StatusMessage ([ComponentModel.Win32Exception]::new(-2146762487).Message)
Assert-FairyflyMsixSignature $signature $candidate $cert
Check $true 'Windows 10 untrusted root status permits matching certificate trust'
$signature.StatusMessage = 'Unrelated unknown failure'
Refuses { Assert-FairyflyMsixSignature $signature $candidate $cert } 'signature'

# Parse the real MSIX container format, including a refusal of external XML entities.
Add-Type -AssemblyName System.IO.Compression.FileSystem
$temp = Join-Path ([IO.Path]::GetTempPath()) ('ffy-update-' + [guid]::NewGuid() + '.msix')
try {
    $zip = [IO.Compression.ZipFile]::Open($temp, 'Create')
    $writer = [IO.StreamWriter]::new($zip.CreateEntry('AppxManifest.xml').Open())
    $writer.Write('<Package xmlns="http://schemas.microsoft.com/appx/manifest/foundation/windows10"><Identity Name="DataZoo.fairyfly" Publisher="CN=05AB657A-5AC7-43BC-A7F1-92AAD620BCCC" Version="2026.10.3000.0" ProcessorArchitecture="x64"/></Package>')
    $writer.Dispose(); $zip.Dispose()
    $read = Get-FairyflyMsixIdentity $temp
    Check ($read.Version -eq [version]'2026.10.3000.0') 'manifest version read'
    Assert-FairyflyMsixUpdate $read $null
    Check $true 'real container parsed'
    $zip = [IO.Compression.ZipFile]::Open($temp, 'Update')
    $zip.GetEntry('AppxManifest.xml').Delete()
    $writer = [IO.StreamWriter]::new($zip.CreateEntry('AppxManifest.xml').Open())
    $writer.Write('<!DOCTYPE Package [<!ENTITY external SYSTEM "file:///never-read-this">]><Package>&external;</Package>')
    $writer.Dispose(); $zip.Dispose()
    Refuses { Get-FairyflyMsixIdentity $temp } 'DTD'
} finally { Remove-Item -LiteralPath $temp -ErrorAction SilentlyContinue }
Write-Host "$script:checks updater checks passed"
