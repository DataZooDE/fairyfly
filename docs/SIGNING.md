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
   `YYYY.MM.DD.N` (`2026.10.24.1`, then `2026.10.24.2`, ...). Update `CHANGELOG.md`, merge through a reviewed pull request (`.github/CODEOWNERS` lists the reviewer
   of CI, build and signing files).
2. `git tag vYYYY.MM.DD && git push origin vYYYY.MM.DD` (or `vYYYY.MM.DD.N`). [`release.yml`](../.github/workflows/release.yml) builds from the
   tag on a GitHub-hosted runner, runs the unit tests, checks that the tag equals `fairyfly --version` and the
   executable's `ProductVersion` (and that `ProductName` is `fairyfly`), signs with Azure Artifact Signing when
   `AZURE_SIGNING_CLIENT_SECRET` exists, verifies the signature with `signtool verify /pa`, and creates the GitHub release
   with `fairyfly.exe` and `SHA256SUMS`. Without the secret the release is published unsigned. A same-day release
   `YYYY.MM.DD.N` is a normal update: `fairyfly update` takes it over `YYYY.MM.DD`, and its MSI (`26.10.24.1`)
   replaces the installed `26.10.24` (Windows Installer ignores the 4th field, `AllowSameVersionUpgrades` covers it;
   proven in CI by `winget.yml`), as the next day's MSI replaces it.
3. Verify a download: `signtool verify /pa /v fairyfly.exe` and `Get-FileHash fairyfly.exe` against `SHA256SUMS`.

The signing step was written from the documentation of the Azure signing action and could not be run before the Azure
account exists: the exact input names may need small corrections.

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
