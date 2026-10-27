<#
.SYNOPSIS
  Checks packaging/sign/Test-Signature.ps1, the signature check of the release (CI: .github/workflows/signing.yml;
  pwsh 7.4+ on Windows, no install, no secrets).
.DESCRIPTION
  Test-Signature.ps1 must refuse every file that is not what release.yml ships: an unsigned file, a file signed by a
  certificate that does not chain to a trusted root (even with O=DataZoo GmbH, C=DE in its subject), a signature
  without an RFC 3161 time stamp, and a signer other than the expected O= and C=. It must accept a file with a valid,
  time-stamped Authenticode signature of the expected publisher; that is shown with pwsh.exe (signed by
  O=Microsoft Corporation, C=US), the one validly signed file every runner has.

  The cases with a trusted test root (no time stamp, wrong O=, wrong C=, and a positive DataZoo case) add a
  self-signed certificate to LocalMachine\Root and remove it again; they run only on CI runners ($env:CI = 'true')
  or with -TrustTestRoot, never by accident on a workstation.
#>
param([switch]$TrustTestRoot)
#Requires -Version 7.4
$ErrorActionPreference = 'Stop'
$checker = Join-Path (Split-Path $PSScriptRoot -Parent) 'Test-Signature.ps1'
$failures = 0
function Check([bool]$cond, [string]$msg) {
    if ($cond) { Write-Host "PASS: $msg" } else { Write-Host "FAIL: $msg"; $script:failures++ }
}
function Invoke-Check([string[]]$Path, [string]$Organization = 'DataZoo GmbH', [string]$Country = 'DE') {
    try { & $checker -Path $Path -ExpectedOrganization $Organization -ExpectedCountry $Country | Out-Host; return '' }
    catch { Write-Host "  refused: $($_.Exception.Message)"; return $_.Exception.Message }
}
function Expect-Refused([string]$name, [string]$pattern, [string[]]$Path, [string]$Organization = 'DataZoo GmbH', [string]$Country = 'DE') {
    $err = Invoke-Check -Path $Path -Organization $Organization -Country $Country
    Check ($err -and $err -match $pattern) "$name is refused with /$pattern/ (got: '$err')"
}

$work = Join-Path ([IO.Path]::GetTempPath()) "ffy-sigtest-$PID"
New-Item -ItemType Directory -Force $work | Out-Null
$certs = @()
try {
    # an unsigned PE file (a DLL is a PE image like fairyfly.exe; Set-AuthenticodeSignature signs it the same way)
    $unsigned = Join-Path $work 'unsigned.dll'
    Add-Type -TypeDefinition 'public static class FfySigTest { public static int Answer() { return 42; } }' -OutputAssembly $unsigned -OutputType Library
    function New-Copy([string]$name) { $p = Join-Path $work $name; Copy-Item $unsigned $p; return $p }

    Write-Host '=== unsigned and missing files ==='
    Expect-Refused 'an unsigned PE file' 'not signed' -Path $unsigned
    Expect-Refused 'a missing file' 'not found' -Path (Join-Path $work 'nope.exe')
    Expect-Refused 'a list with one unsigned file' 'not signed' -Path @("$PSHOME\pwsh.exe", $unsigned) -Organization 'Microsoft Corporation' -Country 'US'

    Write-Host '=== a self-signed certificate that no root trusts ==='
    # the subject is the expected one: the refusal must come from the chain, not from the name
    $self = New-SelfSignedCertificate -Type CodeSigningCert -Subject 'CN=DataZoo GmbH, O=DataZoo GmbH, C=DE' -CertStoreLocation Cert:\CurrentUser\My
    $certs += $self
    $selfSigned = New-Copy 'self-signed.dll'
    $r = Set-AuthenticodeSignature -FilePath $selfSigned -Certificate $self -HashAlgorithm SHA256
    Check ($null -ne $r.SignerCertificate) "the test file was signed with the self-signed certificate ($($r.Status))"
    Expect-Refused 'a self-signed DataZoo-named signature' 'does not verify' -Path $selfSigned

    Write-Host '=== a validly signed, time-stamped file (pwsh.exe, Microsoft) ==='
    $pwshExe = Join-Path $PSHOME 'pwsh.exe'
    $ok = Invoke-Check -Path $pwshExe -Organization 'Microsoft Corporation' -Country 'US'
    Check ($ok -eq '') "pwsh.exe passes as O=Microsoft Corporation, C=US (got: '$ok')"
    $ok = Invoke-Check -Path $pwshExe -Organization '  microsoft   CORPORATION ' -Country 'us'
    Check ($ok -eq '') "O= and C= are compared normalised like the updater (src/update signer_matches) (got: '$ok')"
    Expect-Refused 'pwsh.exe against O=DataZoo GmbH' 'expected O=DataZoo GmbH' -Path $pwshExe
    Expect-Refused 'pwsh.exe against C=DE' 'expected .*C=DE' -Path $pwshExe -Organization 'Microsoft Corporation' -Country 'DE'
    try { & $checker -Path $pwshExe | Out-Host; $err = '' } catch { $err = $_.Exception.Message }
    Check ($err -match 'expected O=DataZoo GmbH, C=DE') "without -Expected* the updater pin of CMakeLists.txt is the default (got: '$err')"
    $ok = Invoke-Check -Path $pwshExe -Organization 'Microsoft Corporation' -Country ''
    Check ($ok -eq '') "-ExpectedCountry '' does not check the country (got: '$ok')"

    if ($env:CI -eq 'true' -or $TrustTestRoot) {
        Write-Host '=== a trusted test root (LocalMachine\Root, removed afterwards) ==='
        function New-TrustedSigned([string]$subject, [string]$name, [switch]$Timestamp) {
            $c = New-SelfSignedCertificate -Type CodeSigningCert -Subject $subject -CertStoreLocation Cert:\CurrentUser\My
            $script:certs += $c
            $cer = Join-Path $work "$name.cer"
            Export-Certificate -Cert $c -FilePath $cer | Out-Null
            Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\Root | Out-Null
            Import-Certificate -FilePath $cer -CertStoreLocation Cert:\LocalMachine\TrustedPublisher | Out-Null
            $f = New-Copy "$name.dll"
            $sp = @{ FilePath = $f; Certificate = $c; HashAlgorithm = 'SHA256' }
            if ($Timestamp) { $sp.TimestampServer = 'http://timestamp.digicert.com' }
            $s = Set-AuthenticodeSignature @sp
            if ($s.Status -ne 'Valid') { throw "signing the test file $name failed: $($s.Status) $($s.StatusMessage)" }
            return $f
        }
        $noTs = New-TrustedSigned 'CN=DataZoo GmbH, O=DataZoo GmbH, C=DE' 'trusted-no-ts'
        Expect-Refused 'a trusted DataZoo signature without a time stamp' 'time stamp' -Path $noTs
        $other = New-TrustedSigned 'CN=Example Ltd, O=Example Ltd, C=DE' 'trusted-other-o' -Timestamp
        Expect-Refused 'a trusted, time-stamped signature of another organisation' 'expected O=DataZoo GmbH' -Path $other
        $otherC = New-TrustedSigned 'CN=DataZoo GmbH, O=DataZoo GmbH, C=AT' 'trusted-other-c' -Timestamp
        Expect-Refused 'a trusted, time-stamped DataZoo signature with C=AT' 'expected .*C=DE' -Path $otherC
        $twoO = New-TrustedSigned 'CN=DataZoo GmbH, O=DataZoo GmbH, O=Example Ltd, C=DE' 'trusted-two-o' -Timestamp
        Expect-Refused 'a subject with two O= values' 'exactly one O=' -Path $twoO
        $good = New-TrustedSigned 'CN=DataZoo GmbH, O=DataZoo GmbH, L=Stuttgart, C=DE' 'trusted-good' -Timestamp
        $ok = Invoke-Check -Path $good
        Check ($ok -eq '') "a trusted, time-stamped O=DataZoo GmbH, C=DE signature passes, whatever L= says (got: '$ok')"
        try { & $checker -Path $good | Out-Host; $ok = '' } catch { $ok = $_.Exception.Message }
        Check ($ok -eq '') "without -Expected* the pin of CMakeLists.txt (DataZoo GmbH, DE) is used and the file passes (got: '$ok')"
        $ok = Invoke-Check -Path @($good, $pwshExe) -Organization 'DataZoo GmbH'
        Check ($ok -match 'expected O=DataZoo GmbH' -and $ok -notmatch 'trusted-good') "with several files only the bad one is reported (got: '$ok')"
    } else {
        Write-Host 'SKIP: trusted-test-root cases (not on CI; pass -TrustTestRoot to run them, needs an elevated shell)'
    }
}
finally {
    foreach ($c in $certs) {
        foreach ($store in 'Cert:\CurrentUser\My', 'Cert:\LocalMachine\Root', 'Cert:\LocalMachine\TrustedPublisher') {
            Remove-Item (Join-Path $store $c.Thumbprint) -ErrorAction SilentlyContinue
        }
    }
    Remove-Item $work -Recurse -Force -ErrorAction SilentlyContinue
}

if ($failures) { Write-Host "$failures check(s) failed"; exit 1 }
Write-Host 'all signature checks passed'
