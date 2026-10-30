# Code signing

The releases are not signed yet: [`release.yml`](../.github/workflows/release.yml) signs them as soon as the one-time
setup below is done, and publishes them unsigned until then. Unsigned, the Windows UAC prompt of `fairyfly mcp setup` says
"Verified publisher: Unknown". With an Authenticode signature that chains to a certificate Windows trusts, it shows the
publisher name instead.

## Which route

fairyfly is licensed under the **Business Source License 1.1** (see [LICENSE](../LICENSE), same terms as DataZooDE/erpl).
That licence is **not OSI-approved**, so the free **SignPath Foundation** programme for open-source projects
(<https://signpath.org/terms.html>: OSI-approved licence, no commercial restrictions) is **not available**. Remaining options:

| Option | Cost | Publisher shown in UAC | Notes |
|---|---|---|---|
| **Azure Artifact Signing** (formerly Trusted Signing) | about 10 USD/month (Basic) | the validated legal name, for example `DataZoo GmbH` | needs a paid Azure subscription and identity validation of the legal entity (organizations in the EU are supported); recommended |
| OV/EV code-signing certificate from a CA | roughly 200-500 EUR/year, hardware token or cloud HSM required since 2023 | the certificate subject | fallback (e.g. SSL.com OV with eSigner); EV no longer bypasses SmartScreen |
| Self-signed certificate | free | your own name, **only on machines that trust the certificate** | for local testing only, never distribute |

## Microsoft Artifact Signing: one-time setup

Nothing below exists yet. Until step 9 is done, `release.yml` behaves as before and publishes unsigned releases.
Signing needs **no secret**: the release job logs in to Azure with GitHub's OIDC token (federated credential).

1. **Paid Azure subscription** owned by DataZoo GmbH (pay-as-you-go or EA; free, trial and sponsored subscriptions are
   refused). Then `az provider register --namespace Microsoft.CodeSigning`.
2. **Artifact Signing account in West Europe**, Basic SKU (9.99 USD/month, billed per full month from creation, so
   create it only when step 3 can follow the same week):

   ```sh
   az group create -n rg-codesigning -l westeurope
   az extension add --name artifact-signing
   az artifact-signing create -n datazoosigning -g rg-codesigning -l westeurope --sku Basic
   ```

   The account name is 3-24 alphanumeric characters and globally unique. Its endpoint is
   `https://weu.codesigning.azure.net` (it must match the region, otherwise signing fails with 403).
3. **Identity validation** (portal only, by a person with the role *Artifact Signing Identity Verifier* on the
   account): *Identity validations* > *New* > *Organization*, *Public*. Organization name exactly `DataZoo GmbH` as in the
   Handelsregister; website and the primary and secondary e-mail on the same company domain; business identifier =
   HRB number and court; registered address. Check the certificate subject preview, it must show `O=DataZoo GmbH` and
   `C=DE` (the updater accepts nothing else, see [Updates](#updates)). Click the e-mail link within 7 days, do the
   representative's Verified ID (ID card, Microsoft Authenticator) and upload documents younger than 12 months if
   asked: three attempts, then the validation fails for good. It takes 1-20 business days.
4. **Certificate profile**, type **Public Trust**, e.g. `fairyfly-public`, on the completed validation. Leave "include
   street address" and "include postal code" unticked (fewer fields that a later re-validation could change):
   `az artifact-signing certificate-profile create -g rg-codesigning --account-name datazoosigning -n fairyfly-public --profile-type PublicTrust --identity-validation-id <id>`.
5. **Entra app registration** `fairyfly-release-signing`, **without a client secret**, with a **flexible federated
   identity credential** (Entra, preview, verified 2026-10-27 on
   [Microsoft Learn](https://learn.microsoft.com/en-us/entra/workload-id/workload-identities-flexible-federated-identity-credentials):
   for GitHub it matches `sub` and `job_workflow_ref` with `eq`/`matches`, and requires `repository_id` or
   `repository_owner_id`). A plain credential with only the subject `repo:DataZooDE/fairyfly:environment:release-signing`
   would give a signing token to **any** workflow file that names the environment, for example one pushed on a
   branch together with a tag that passes the environment rule; the expression below also binds the token to
   `release.yml` run from a `v*` tag of this repository (ids: repository 1084036170, owner 136052936). `az` cannot
   create or read flexible credentials yet, so use Microsoft Graph (beta) through `az rest`, with the app's
   **object id**:

   ```sh
   cat > fic.json <<'JSON'
   {"name": "fairyfly-release-yml-tags",
    "issuer": "https://token.actions.githubusercontent.com",
    "audiences": ["api://AzureADTokenExchange"],
    "claimsMatchingExpression": {"languageVersion": 1,
      "value": "claims['sub'] eq 'repo:DataZooDE/fairyfly:environment:release-signing' and claims['repository_id'] eq '1084036170' and claims['repository_owner_id'] eq '136052936' and claims['job_workflow_ref'] matches 'DataZooDE/fairyfly/.github/workflows/release.yml@refs/tags/v*'"}}
   JSON
   az rest --method post --url "https://graph.microsoft.com/beta/applications/<appObjectId>/federatedIdentityCredentials" \
     --headers Content-Type=application/json --body @fic.json
   ```

   If the repository later opts into GitHub's immutable subject format, `sub` changes and the expression must follow.
   Do not add a second, plain (subject-only) credential: it would undo the binding.
6. **Role**: assign **Artifact Signing Certificate Profile Signer** to the app's service principal on the
   *certificate profile* (the narrowest scope; no subscription role is needed):
   `az role assignment create --assignee <appId> --role "Artifact Signing Certificate Profile Signer" --scope <profile resource id>`.
7. **Only admins create release tags, and a person approves every signing run.**
   - **Tag ruleset** (otherwise anyone with write access can push a `v*` tag on a commit with a changed `release.yml`):
     creating, moving and deleting `v*` tags is reserved to repository admins (role id 5):

     ```sh
     gh api -X POST repos/DataZooDE/fairyfly/rulesets --input - <<'JSON'
     {"name": "release tags: admins only", "target": "tag", "enforcement": "active",
      "conditions": {"ref_name": {"include": ["refs/tags/v*"], "exclude": []}},
      "rules": [{"type": "creation"}, {"type": "update"}, {"type": "deletion"}],
      "bypass_actors": [{"actor_id": 5, "actor_type": "RepositoryRole", "bypass_mode": "always"}]}
     JSON
     ```

   - **GitHub environment** `release-signing` (Settings > Environments): deployment branches and tags = **selected,
     tag rule `v*` only**, and **required reviewers: mandatory** (e.g. `@jrosskopf`). The sign job then waits for an
     approval before it can obtain a token; the reviewer checks that the run is `release.yml` on the expected tag.
   - A manual run (`workflow_dispatch`) has no tag input; it must run on the tag itself:
     `gh workflow run release.yml --ref vYYYY.MM.DD`. The build and sign jobs refuse any run whose `github.ref` is not
     `refs/tags/<tag>`, and every job checks out `github.sha`, the commit of that tag
     (`packaging/sign/tests/Test-ReleaseWorkflow.ps1` guards these rules, and that every action in `release.yml` is
     pinned to a commit SHA).
8. **Repository variables** (Settings > Secrets and variables > Actions > *Variables*, not secrets):

   | Variable | Value |
   |---|---|
   | `AZURE_SIGNING_TENANT_ID` | Entra tenant ID |
   | `AZURE_SIGNING_CLIENT_ID` | application (client) ID of the app from step 5 |
   | `AZURE_SIGNING_SUBSCRIPTION_ID` | subscription ID from step 1 |
   | `AZURE_SIGNING_ENDPOINT` | `https://weu.codesigning.azure.net` |
   | `AZURE_SIGNING_PROFILE` | `fairyfly-public` |
   | `AZURE_SIGNING_ACCOUNT` | `datazoosigning`. **Set this one last**: a non-empty `AZURE_SIGNING_ACCOUNT` switches signing on, and the release then fails if any other variable above is empty. |

   e.g. `gh variable set AZURE_SIGNING_ENDPOINT --repo DataZooDE/fairyfly --body https://weu.codesigning.azure.net`.
   No secret is needed; an old `AZURE_SIGNING_CLIENT_SECRET` is not used and can be deleted.
9. Tag the next release. Check the sign job's output of `Test-Signature.ps1` (it prints the exact signer subject and
   chain), install the MSI, and run `fairyfly update apply` from the last unsigned release. Then set the variable
   **`REQUIRE_SIGNING=true`**: from then on a release whose signing is not configured fails instead of going out
   unsigned.

What the release then does ([`release.yml`](../.github/workflows/release.yml), job `sign`):
`azure/login` (OIDC) > `azure/artifact-signing-action` (pinned to the v2.0.0 commit) signs every exe of the build
(`fairyfly.exe`, `fairyfly-tray.exe`) with an RFC 3161 time stamp from `http://timestamp.acs.microsoft.com` >
[`packaging/sign/Test-Signature.ps1`](../packaging/sign/Test-Signature.ps1) checks them > the MSI is built around the
signed exes, signed and checked the same way. `Test-Signature.ps1` refuses a file unless its embedded signature is
valid and chains to a trusted root (`Get-AuthenticodeSignature` and `signtool verify /pa`), it has a time stamp (an
Artifact Signing certificate lives 3 days), and the signer has exactly one `O=` and one `C=`, equal to the updater pin
(`FAIRYFLY_UPDATE_SIGNER_ORGANIZATION`/`_COUNTRY` in `CMakeLists.txt`: `DataZoo GmbH`, `DE`). Any refusal fails the
release before anything is published. The winget manifests and `SHA256SUMS` are made from the signed files, and the
release job checks that the files it publishes are the ones that were verified. The script's tests
(`packaging/sign/tests/Test-Signature.Tests.ps1`, workflow `signing.yml`) need no secret: an unsigned file, a
self-signed certificate, a missing time stamp and wrong `O=`/`C=` are refused, a valid signature passes.

## Releasing

1. Bump the calendar version with `./packaging/set_version.ps1 -Version YYYY.MM.DD` (it sets `src/include/version.h`,
   `CMakeLists.txt` and `vcpkg.json`); for a further release on a day that already has one, use the next
   `YYYY.MM.DD.N` (`2026.10.24.1`, then `2026.10.24.2`, ... up to `.99`). Update `CHANGELOG.md`, merge through a reviewed pull request (`.github/CODEOWNERS` lists the reviewer
   of CI, build and signing files).
2. `git tag vYYYY.MM.DD && git push origin vYYYY.MM.DD` (or `vYYYY.MM.DD.N`). [`release.yml`](../.github/workflows/release.yml) builds from the
   tag on a GitHub-hosted runner, runs the unit tests, checks that the tag equals `fairyfly --version` and the
   executable's `ProductVersion` (and that `ProductName` is `fairyfly`), signs and verifies the exes and the MSI when the
   variable `AZURE_SIGNING_ACCOUNT` is set (see above), and creates the GitHub release with `fairyfly.exe`,
   `fairyfly.msi`, the winget manifests, the unsigned Microsoft Store package `fairyfly-<msixversion>.msix` (not
   signed by us: the Store signs it; its version must rise with every release, [MSIX.md](MSIX.md#store-submission))
   and `SHA256SUMS`. Without `AZURE_SIGNING_ACCOUNT` the release is published
   unsigned (a notice in the run says so), or fails when `REQUIRE_SIGNING` is `true`. A same-day release
   `YYYY.MM.DD.N` is a normal update: `fairyfly update` takes it over `YYYY.MM.DD`, and its MSI (`26.10.2401` for
   `2026.10.24.1`, `(YYYY-2000).MM.(DD*100+N)`) replaces the installed `26.10.2400`, as the next day's MSI
   (`26.10.2500`) replaces it; the older MSI is refused over a newer one (downgrade). Proven in CI by `winget.yml`,
   starting from the released `v2026.10.24.1` MSI (`26.10.24.1`, the old scheme).
3. Verify a download: `signtool verify /pa /v fairyfly.msi` (or `Get-AuthenticodeSignature .\fairyfly.msi | Format-List
   Status, SignerCertificate, TimeStamperCertificate`), or `./packaging/sign/Test-Signature.ps1 -Path fairyfly.exe, fairyfly.msi`,
   and `Get-FileHash fairyfly.msi` against `SHA256SUMS`.

The inputs of the signing action were checked against its `action.yml` at v2.0.0, but the signed path cannot run
before the Azure account exists; the first signed release is its end-to-end test (step 9 above).

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
- SmartScreen reputation builds up over downloads of signed files, per publisher and file. A signature changes
  "Unknown publisher" into "DataZoo GmbH" from the first release, but the SmartScreen prompt stays until the
  reputation exists (weeks). EV certificates no longer skip that wait (Microsoft, SmartScreen reputation for app
  developers).
