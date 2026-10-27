# Code signing

`fairyfly.exe` is not signed yet. Unsigned, the Windows UAC prompt of `fairyfly mcp setup` says
"Verified publisher: Unknown". With an Authenticode signature that chains to a certificate Windows trusts, it shows the
publisher name instead.

## Which route

fairyfly is licensed under the **Business Source License 1.1** (see [LICENSE](../LICENSE), same terms as DataZooDE/erpl).
That licence is **not OSI-approved**, so the free **SignPath Foundation** programme for open-source projects
(<https://signpath.org/terms.html>: OSI-approved licence, no commercial restrictions) is **not available**. Remaining options:

| Option | Cost | Publisher shown in UAC | Notes |
|---|---|---|---|
| **Azure Artifact Signing** (formerly Trusted Signing) | about 10 USD/month (Basic) | the validated legal name, for example `DataZoo GmbH` | needs a paid Azure subscription and identity validation of the legal entity (organizations in the EU are supported); recommended |
| OV/EV code-signing certificate from a CA | roughly 200-500 EUR/year, hardware token or cloud HSM required since 2023 | the certificate subject | EV additionally removes SmartScreen warnings for downloads |
| Self-signed certificate | free | your own name, **only on machines that trust the certificate** | for local testing only, never distribute |

## Azure Artifact Signing: what is needed

1. Paid Azure subscription (free, trial and sponsored subscriptions are not supported).
2. Create an **Artifact Signing account** in a supported region (portal or `az`), then complete **identity validation**
   for the organization: legal entity name exactly as in the commercial register (DataZoo GmbH), plus the documents the
   portal asks for; plan a few business days.
3. Create a **certificate profile** of type *Public Trust* (this is what makes the publisher verified).
4. Create an Entra app registration (service principal) for CI and assign it the role
   *Artifact Signing Certificate Profile Signer* (formerly *Trusted Signing Certificate Profile Signer*) on the profile.
5. In the GitHub repository set the **variables** `AZURE_SIGNING_TENANT_ID`, `AZURE_SIGNING_CLIENT_ID`,
   `AZURE_SIGNING_ENDPOINT` (the regional endpoint of the account), `AZURE_SIGNING_ACCOUNT`, `AZURE_SIGNING_PROFILE`,
   and the **secret** `AZURE_SIGNING_CLIENT_SECRET`.

## Releasing

1. Bump the calendar version with `./packaging/set_version.ps1 -Version YYYY.MM.DD` (it sets `src/include/version.h`,
   `CMakeLists.txt` and `vcpkg.json`); for a further release on a day that already has one, use the next
   `YYYY.MM.DD.N` (`2026.10.24.1`, then `2026.10.24.2`, ... up to `.99`). Update `CHANGELOG.md`, merge through a reviewed pull request (`.github/CODEOWNERS` lists the reviewer
   of CI, build and signing files).
2. `git tag vYYYY.MM.DD && git push origin vYYYY.MM.DD` (or `vYYYY.MM.DD.N`). [`release.yml`](../.github/workflows/release.yml) builds from the
   tag on a GitHub-hosted runner, runs the unit tests, checks that the tag equals `fairyfly --version` and the
   executable's `ProductVersion` (and that `ProductName` is `fairyfly`), signs with Azure Artifact Signing when
   `AZURE_SIGNING_CLIENT_SECRET` exists, verifies the signature with `signtool verify /pa`, and creates the GitHub release
   with `fairyfly.exe` and `SHA256SUMS`. Without the secret the release is published unsigned. A same-day release
   `YYYY.MM.DD.N` is a normal update: `fairyfly update` takes it over `YYYY.MM.DD`, and its MSI (`26.10.2401` for
   `2026.10.24.1`, `(YYYY-2000).MM.(DD*100+N)`) replaces the installed `26.10.2400`, as the next day's MSI
   (`26.10.2500`) replaces it; the older MSI is refused over a newer one (downgrade). Proven in CI by `winget.yml`,
   starting from the released `v2026.10.24.1` MSI (`26.10.24.1`, the old scheme).
3. Verify a download: `signtool verify /pa /v fairyfly.exe` and `Get-FileHash fairyfly.exe` against `SHA256SUMS`.

The signing step was written from the documentation of the Azure signing action and could not be run before the Azure
account exists: the exact input names may need small corrections.

## Updates

`fairyfly update apply` (src/update/update_model.cpp, `decide()`) accepts a release only when its `fairyfly.exe`
matches `SHA256SUMS` and its signature passes these rules:

| This installation | The release | Result |
|---|---|---|
| unsigned (self-built, or a release up to 2026.10.x) | unsigned | refused (`UPDATE_UNSIGNED`); with `--allow-unsigned` installed, with a warning |
| unsigned | signed by `O=DataZoo GmbH, C=DE` | installed: the path to signed releases |
| unsigned | signed by anybody else | refused (`UPDATE_SIGNER_UNEXPECTED`), also with `--allow-unsigned` |
| signed | unsigned | refused (`UPDATE_SIGNATURE_INVALID`), also with `--allow-unsigned` |
| signed | signed by anybody else | refused (`UPDATE_SIGNER_UNEXPECTED`) |
| signed | signed by `O=DataZoo GmbH, C=DE` with another address, OU or CN | installed |
| any | a signature that does not verify (expired without a time stamp, revoked, tampered) | refused (`UPDATE_SIGNATURE_INVALID`), never treated as unsigned |
| any | does not match `SHA256SUMS`, or no hash for `fairyfly.exe` | refused (`UPDATE_CHECKSUM_MISMATCH`, `UPDATE_CHECKSUM_MISSING`) |

- **What is pinned.** The organisation `O=` and the country `C=` of the signer's leaf certificate, each compared
  normalised (trimmed, inner white space collapsed, case-insensitive); the subject must hold exactly one of each.
  Not the whole subject: Artifact Signing issues a new leaf certificate every day, and a new identity validation
  (for example after a move) changes `L=`, `S=`, street or postal code, which must not lock signed installations out.
  The certificate itself is checked by `WinVerifyTrust` (chain to a root Windows trusts, time stamp, revocation of
  the whole chain).
- **What is not pinned.** The issuing CA or root. Microsoft rotates the Artifact Signing intermediates (EOC CA 01/02,
  then 03/04), so an intermediate pin would break updates; the publicly trusted chain plus the validated
  organisation name is the trust. The durable identity EKU (`1.3.6.1.4.1.311.97.*`) is a possible later addition.
- **Forks.** The pin is a compile-time constant, set with the CMake options `FAIRYFLY_UPDATE_SIGNER_ORGANIZATION`
  (default `DataZoo GmbH`, must not be empty) and `FAIRYFLY_UPDATE_SIGNER_COUNTRY` (default `DE`; empty: the
  country is not checked), e.g. `-DFAIRYFLY_UPDATE_SIGNER_ORGANIZATION="Example Ltd" -DFAIRYFLY_UPDATE_SIGNER_COUNTRY=GB`.
  Note that the update channel (`DataZooDE/fairyfly` releases) is fixed in `update_model.h` as well.
- **Unsigned releases until the first signed one.** The releases up to 2026.10.x are unsigned. Those installations
  keep taking unsigned releases with `--allow-unsigned` (with a warning that only `SHA256SUMS` was checked) and take
  the first signed release without it. Installations older than this rule accept a release signed by any publicly
  trusted publisher; the pinned code reaches them with the next update.
- **Changing the publisher** (a new legal name, or the SSL.com fallback with another `O=`): ship a release, signed by
  the old publisher, whose pin is the new one; installations that skip it are refused with `UPDATE_SIGNER_UNEXPECTED`
  and need a manual install of the MSI.
- **The MSI.** `fairyfly update apply` replaces only `fairyfly.exe`. The MSI (`fairyfly.msi`) is installed by winget or
  `msiexec`, which check its signature themselves.

## Local test with a self-signed certificate (this machine only)

```powershell
$cert = New-SelfSignedCertificate -Type CodeSigningCert -Subject "CN=fairyfly local test" -CertStoreLocation Cert:\CurrentUser\My
# trust it (elevated): the UAC prompt shows the name only on machines that trust the certificate
Export-Certificate -Cert $cert -FilePath fairyfly-test.cer
certutil -addstore Root fairyfly-test.cer ; certutil -addstore TrustedPublisher fairyfly-test.cer
signtool sign /fd SHA256 /sha1 $cert.Thumbprint build\Release\fairyfly.exe
```

## Notes

- A signature belongs to one build: rebuilding locally produces an unsigned binary again.
- SmartScreen reputation of a newly signed file builds up over downloads; EV certificates avoid the wait.
