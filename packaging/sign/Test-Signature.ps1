<#
.SYNOPSIS
  Checks the Authenticode signature of files that release.yml ships; throws when any of them is not acceptable.
.DESCRIPTION
  For every file in -Path (fairyfly.exe, fairyfly-tray.exe, fairyfly.msi):
    1. it exists and carries an embedded Authenticode signature (a catalog signature does not travel with the file);
    2. the signature verifies and chains to a root Windows trusts (Get-AuthenticodeSignature Status Valid, and
       signtool verify /pa, the Authenticode policy that Windows and winget apply);
    3. it has an RFC 3161 time stamp (an Artifact Signing certificate is valid for 3 days; without a time stamp the
       signature would stop verifying after that);
    4. the signer's leaf certificate has exactly one O= and one C=, equal to -ExpectedOrganization and
       -ExpectedCountry, compared normalised (trimmed, inner white space collapsed, case-insensitive). This is the
       rule of the updater (src/update/update_model.cpp, signer_matches): a release that fails here would be refused
       by `fairyfly update apply`. Both default to that pin, FAIRYFLY_UPDATE_SIGNER_ORGANIZATION and
       FAIRYFLY_UPDATE_SIGNER_COUNTRY in CMakeLists.txt (DataZoo GmbH, DE), so the release is checked against the
       publisher the exe it ships will demand of its own updates. -ExpectedCountry '' skips the country.
  All files are checked; the error lists every refused file. The signer, issuer chain and time stamp of each
  accepted file are printed (the first signed release shows the exact subject).
  CI: release.yml (sign job); tests: packaging/sign/tests/Test-Signature.Tests.ps1 (signing.yml).
.EXAMPLE
  ./packaging/sign/Test-Signature.ps1 -Path fairyfly.exe, fairyfly.msi    # O=DataZoo GmbH, C=DE from CMakeLists.txt
#>
#Requires -Version 7.4
param(
    [Parameter(Mandatory)][string[]]$Path,
    [string]$ExpectedOrganization,
    [string]$ExpectedCountry,
    [string]$CMakeLists = (Join-Path $PSScriptRoot '../../CMakeLists.txt')
)
$ErrorActionPreference = 'Stop'

# by default the updater's pin of this source tree: FAIRYFLY_UPDATE_SIGNER_ORGANIZATION / _COUNTRY in CMakeLists.txt
function Get-CMakeDefault([string]$name) {
    $m = Select-String -LiteralPath $CMakeLists -Pattern "^\s*set\(\s*$name\s+`"([^`"]*)`"" | Select-Object -First 1
    if (-not $m) { throw "$name not found in $CMakeLists" }
    return $m.Matches[0].Groups[1].Value
}
if (-not $PSBoundParameters.ContainsKey('ExpectedOrganization')) { $ExpectedOrganization = Get-CMakeDefault 'FAIRYFLY_UPDATE_SIGNER_ORGANIZATION' }
if (-not $PSBoundParameters.ContainsKey('ExpectedCountry')) { $ExpectedCountry = Get-CMakeDefault 'FAIRYFLY_UPDATE_SIGNER_COUNTRY' }

function Normalize([string]$s) { return (($s ?? '').Trim() -replace '\s+', ' ').ToLowerInvariant() }

if (-not (Normalize $ExpectedOrganization)) { throw 'ExpectedOrganization is empty' }
$expected = "O=$ExpectedOrganization$(if (Normalize $ExpectedCountry) { ", C=$ExpectedCountry" })"

# the values of one attribute (OID) in a distinguished name; a multi-valued RDN is refused (fail closed)
function Get-NameValues([System.Security.Cryptography.X509Certificates.X500DistinguishedName]$dn, [string]$oid) {
    $values = @()
    foreach ($rdn in $dn.EnumerateRelativeDistinguishedNames()) {
        if ($rdn.HasMultipleElements) { throw "the signer's subject has a multi-valued RDN: $($dn.Name)" }
        if ($rdn.GetSingleElementType().Value -eq $oid) { $values += $rdn.GetSingleElementValue() }
    }
    return , $values
}

$signtool = Get-ChildItem "${env:ProgramFiles(x86)}\Windows Kits\10\bin" -Recurse -Filter signtool.exe -ErrorAction SilentlyContinue |
    Where-Object { $_.FullName -like '*\x64\*' } | Sort-Object FullName | Select-Object -Last 1
if (-not $signtool) { throw 'signtool.exe (Windows SDK, x64) not found' }

$refused = @()
foreach ($p in $Path) {
    $why = $null
    try {
        if (-not (Test-Path -LiteralPath $p -PathType Leaf)) { throw "not found" }
        $file = (Resolve-Path -LiteralPath $p).ProviderPath
        $s = Get-AuthenticodeSignature -LiteralPath $file
        if ($s.Status -eq 'NotSigned' -or -not $s.SignerCertificate) { throw "not signed ($($s.StatusMessage))" }
        if ($s.SignatureType -ne 'Authenticode') { throw "not signed: only a $($s.SignatureType) signature, no embedded Authenticode signature" }
        if ($s.Status -ne 'Valid') { throw "the signature does not verify: $($s.Status) $($s.StatusMessage) (signer $($s.SignerCertificate.Subject))" }

        $out = & $signtool.FullName verify /pa /v $file 2>&1 | Out-String
        if ($LASTEXITCODE -ne 0) { throw "the signature does not verify: signtool verify /pa exit $LASTEXITCODE`n$out" }

        if (-not $s.TimeStamperCertificate) { throw "no RFC 3161 time stamp: the signature dies with the signing certificate (signer $($s.SignerCertificate.Subject))" }

        $subject = $s.SignerCertificate.SubjectName
        $o = Get-NameValues $subject '2.5.4.10'
        if ($o.Count -ne 1) { throw "the signer's subject must have exactly one O=, has $($o.Count): '$($subject.Name)', expected $expected" }
        if ((Normalize $o[0]) -ne (Normalize $ExpectedOrganization)) { throw "signed by '$($subject.Name)', expected $expected" }
        if (Normalize $ExpectedCountry) {
            $c = Get-NameValues $subject '2.5.4.6'
            if ($c.Count -ne 1) { throw "the signer's subject must have exactly one C=, has $($c.Count): '$($subject.Name)', expected $expected" }
            if ((Normalize $c[0]) -ne (Normalize $ExpectedCountry)) { throw "signed by '$($subject.Name)', expected $expected" }
        }

        $chain = [System.Security.Cryptography.X509Certificates.X509Chain]::new()
        $chain.ChainPolicy.RevocationMode = 'NoCheck'   # display only; WinVerifyTrust above did the real check
        [void]$chain.Build($s.SignerCertificate)
        $chainText = ($chain.ChainElements | ForEach-Object { $_.Certificate.Subject }) -join "`n      <- "
        Write-Host "OK  $p"
        Write-Host "    signer:     $($subject.Name)"
        Write-Host "    thumbprint: $($s.SignerCertificate.Thumbprint)"
        Write-Host "    chain:      $chainText"
        Write-Host "    time stamp: $($s.TimeStamperCertificate.Subject)"
    }
    catch { $why = $_.Exception.Message }
    if ($why) { Write-Host "REFUSED  $p : $why"; $refused += "$p : $why" }
}
if ($refused) { throw "signature check failed for $($refused.Count) of $($Path.Count) file(s):`n$($refused -join "`n")" }
