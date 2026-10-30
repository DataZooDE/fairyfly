<#
.SYNOPSIS
Updates or installs the local Fairyfly MSIX without uninstalling it or deleting its data.
.DESCRIPTION
Run as the normal Windows user. Requires a newer DataZoo.fairyfly x64 package.
For a sideload test build, supplying CertificatePath authorizes trusting that exact
package signer in LocalMachine\TrustedPeople. Only that step asks for elevation.
Use -WhatIf to validate and preview. An identical installed build is verified and
restarted; a changed same-version rebuild must have its version incremented first.
This script does not publish to the Store or release the gateway.
.EXAMPLE
.\Update-Msix.ps1 -PackagePath .\fairyfly-2026.10.3000.0-testsigned.msix -CertificatePath .\fairyfly-2026.10.3000.0-testsigned.cer
#>
[CmdletBinding(SupportsShouldProcess=$true)]
param(
    [Parameter(Mandatory)][string]$PackagePath,
    [string]$CertificatePath,
    [switch]$NoStartTray
)
$ErrorActionPreference = 'Stop'
. (Join-Path $PSScriptRoot 'Update-Msix.Common.ps1')
$package = (Resolve-Path -LiteralPath $PackagePath).Path
if ([IO.Path]::GetExtension($package) -ne '.msix') { throw 'PackagePath must name a .msix file' }
$identity = Get-FairyflyMsixIdentity $package
$installed = Get-AppxPackage -Name DataZoo.fairyfly
$sameContent = $false
if ($installed -and [version]$installed.Version -eq $identity.Version) {
    $sameContent = (Get-FairyflyMsixBlockMapHash $package) -eq
        (Get-FileHash -LiteralPath (Join-Path $installed.InstallLocation 'AppxBlockMap.xml') -Algorithm SHA256).Hash
}
Assert-FairyflyMsixUpdate $identity $installed -SameContent $sameContent
$certificate = $null
if ($CertificatePath) {
    $certificateFile = (Resolve-Path -LiteralPath $CertificatePath).Path
    if ([IO.Path]::GetExtension($certificateFile) -ne '.cer') { throw 'CertificatePath must name a public .cer file' }
    $certificate = [Security.Cryptography.X509Certificates.X509Certificate2]::new($certificateFile)
    if ($certificate.HasPrivateKey) { throw 'Supply a public certificate, not a private key' }
}
$signature = Get-AuthenticodeSignature -LiteralPath $package
Assert-FairyflyMsixSignature $signature $identity $certificate
$hash = (Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash
$oldVersion = if ($installed) { [string]$installed.Version } else { '(not installed)' }
Write-Host "Fairyfly: $oldVersion -> $($identity.Version)"
Write-Host "Package: $package"
Write-Host "SHA256: $hash"
Write-Host "Signer: $($signature.SignerCertificate.Thumbprint) ($($signature.Status))"
if (-not $PSCmdlet.ShouldProcess('Current user Fairyfly installation', 'Trust supplied signer if needed, update MSIX in place and restart tray')) { return }

if ([string]$signature.Status -ne 'Valid') {
    # Elevate only the certificate import. Package installation stays in this user's session.
    $helper = Join-Path ([IO.Path]::GetTempPath()) ('ffy-msix-trust-' + [guid]::NewGuid() + '.ps1')
    @'
param([string]$CertificateFile, [string]$ExpectedThumbprint)
$ErrorActionPreference = 'Stop'
try {
    $cert = [Security.Cryptography.X509Certificates.X509Certificate2]::new($CertificateFile)
    if ($cert.Thumbprint -ne $ExpectedThumbprint -or $cert.HasPrivateKey) { throw 'Certificate changed' }
    Import-Certificate -FilePath $CertificateFile -CertStoreLocation Cert:\LocalMachine\TrustedPeople | Out-Null
    exit 0
} catch { exit 1 }
'@ | Set-Content -LiteralPath $helper -Encoding UTF8
    try {
        Write-Host 'Windows will ask for administrator approval to trust this test-package certificate.'
        $arguments = '-NoProfile -ExecutionPolicy Bypass -File "{0}" -CertificateFile "{1}" -ExpectedThumbprint "{2}"' -f $helper, $certificateFile, $certificate.Thumbprint
        $admin = Start-Process -FilePath "$env:SystemRoot\System32\WindowsPowerShell\v1.0\powershell.exe" -Verb RunAs -WindowStyle Hidden -ArgumentList $arguments -Wait -PassThru
        if ($admin.ExitCode -ne 0) { throw 'Certificate import failed; installation was not changed' }
    } finally { Remove-Item -LiteralPath $helper -ErrorAction SilentlyContinue }
}
if ((Get-FileHash -LiteralPath $package -Algorithm SHA256).Hash -ne $hash) { throw 'Package changed during preparation' }
$signature = Get-AuthenticodeSignature -LiteralPath $package
if ([string]$signature.Status -ne 'Valid') { throw "Package signature is still not trusted: $($signature.Status)" }

$running = @()
if ($installed) {
    $executables = @((Join-Path $installed.InstallLocation 'fairyfly.exe'), (Join-Path $installed.InstallLocation 'fairyfly-tray.exe'))
    $running = @(Get-Process -Name fairyfly,fairyfly-tray -ErrorAction SilentlyContinue | Where-Object { $_.Path -in $executables })
}
try {
    foreach ($process in $running) {
        Stop-Process -Id $process.Id -ErrorAction Stop
        if (-not $process.WaitForExit(10000)) { throw 'Fairyfly did not exit; installation was not started' }
    }
    if (-not $sameContent) { Add-AppxPackage -Path $package -ErrorAction Stop }
    $updated = Get-AppxPackage -Name DataZoo.fairyfly
    if (-not $updated -or [version]$updated.Version -ne $identity.Version) { throw 'Installed version did not match the requested package' }
    $alias = Join-Path $env:LOCALAPPDATA "Microsoft\WindowsApps\$($updated.PackageFamilyName)\fairyfly.exe"
    $reported = (& $alias --version | Out-String).Trim()
    if ($LASTEXITCODE -ne 0) { throw 'Installed executable did not start' }
    if (-not $NoStartTray) {
        Start-FairyflyPackagedTray $updated
    }
    [pscustomobject]@{Package=$updated.PackageFullName; Version=$updated.Version; ExecutableVersion=$reported;
        SHA256=$hash; TrayStarted=(-not $NoStartTray); DataPreserved=$true}
} catch {
    $originalError = $_
    # Windows owns the update transaction. Restart whichever version remains registered.
    if ($running.Count -gt 0) {
        try {
            $remaining = Get-AppxPackage -Name DataZoo.fairyfly
            if ($remaining) { Start-FairyflyPackagedTray $remaining }
        } catch { Write-Warning "Tray restart also failed: $($_.Exception.Message)" }
    }
    throw $originalError
}
