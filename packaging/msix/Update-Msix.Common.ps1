function Get-FairyflyMsixIdentity([string]$Path) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($Path)
    try {
        $entry = $zip.GetEntry('AppxManifest.xml')
        if (-not $entry -or $entry.Length -gt 1MB) { throw 'Missing or oversized AppxManifest.xml' }
        $settings = [Xml.XmlReaderSettings]::new()
        $settings.DtdProcessing = [Xml.DtdProcessing]::Prohibit
        $settings.XmlResolver = $null
        $stream = $entry.Open()
        $reader = [Xml.XmlReader]::Create($stream, $settings)
        try {
            $xml = [Xml.XmlDocument]::new()
            $xml.XmlResolver = $null
            $xml.Load($reader)
            $id = $xml.Package.Identity
            if (-not $id) { throw 'Package identity is missing' }
            return [pscustomobject]@{Name=[string]$id.Name; Publisher=[string]$id.Publisher;
                Version=[version]$id.Version; Architecture=[string]$id.ProcessorArchitecture}
        } finally { $reader.Dispose(); $stream.Dispose() }
    } finally { $zip.Dispose() }
}

function Get-FairyflyMsixBlockMapHash([string]$Path) {
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($Path)
    try {
        $entry = $zip.GetEntry('AppxBlockMap.xml')
        if (-not $entry) { throw 'Package block map missing' }
        $stream = $entry.Open()
        $sha = [Security.Cryptography.SHA256]::Create()
        try { return [BitConverter]::ToString($sha.ComputeHash($stream)).Replace('-', '') }
        finally { $sha.Dispose(); $stream.Dispose() }
    } finally { $zip.Dispose() }
}

function Start-FairyflyPackagedTray($Package) {
    Invoke-CommandInDesktopPackage -PackageFamilyName $Package.PackageFamilyName -AppId Tray `
        -Command (Join-Path $Package.InstallLocation 'fairyfly-tray.exe') -PreventBreakaway
}

function Assert-FairyflyMsixUpdate($Candidate, $Installed, [bool]$SameContent=$false) {
    if ($Candidate.Name -cne 'DataZoo.fairyfly') { throw 'Unexpected package identity; expected DataZoo.fairyfly' }
    if ($Candidate.Publisher -cne 'CN=05AB657A-5AC7-43BC-A7F1-92AAD620BCCC') { throw 'Unexpected package publisher' }
    if ($Candidate.Architecture -ne 'x64') { throw 'Expected x64 package architecture' }
    if ($Installed) {
        if ($Installed.Name -cne $Candidate.Name -or $Installed.Publisher -cne $Candidate.Publisher) {
            throw 'Installed package identity does not match'
        }
        if ($Candidate.Version -eq [version]$Installed.Version -and $SameContent) { return }
        if ($Candidate.Version -le [version]$Installed.Version) {
            throw "Build a higher version than $($Installed.Version) for an in-place replacement; this script does not uninstall or downgrade."
        }
    }
}

function Assert-FairyflyMsixSignature($Signature, $Identity, $Certificate) {
    if (-not $Signature.SignerCertificate -or $Signature.SignerCertificate.Subject -cne $Identity.Publisher) {
        throw 'Package signature does not match its publisher'
    }
    # Windows PowerShell 5.1 reports CERT_E_UNTRUSTEDROOT as UnknownError. Match
    # Windows' localized message for that exact HRESULT, never arbitrary errors.
    $untrustedRootMessage = [ComponentModel.Win32Exception]::new(-2146762487).Message
    $untrusted = [string]$Signature.Status -eq 'NotTrusted' -or
        ([string]$Signature.Status -eq 'UnknownError' -and
         [string]$Signature.StatusMessage -eq $untrustedRootMessage)
    if ([string]$Signature.Status -ne 'Valid' -and -not $untrusted) { throw "Invalid package signature: $($Signature.Status)" }
    if ($Certificate -and ($Certificate.Thumbprint -ne $Signature.SignerCertificate.Thumbprint -or $Certificate.Subject -cne $Identity.Publisher)) {
        throw 'Certificate does not match the package signer'
    }
    if ($untrusted -and -not $Certificate) {
        throw 'Signer is not trusted. Supply -CertificatePath with this test build''s matching public .cer file.'
    }
}
