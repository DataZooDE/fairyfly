# Code signing (SignPath Foundation)

fairyfly applies for **free code signing from the [SignPath Foundation](https://signpath.org/)** (open-source program).
Then the Windows UAC prompt of `fairyfly mcp setup` shows "Verified publisher: SignPath Foundation" instead of
"Unknown". Conditions: <https://signpath.org/terms.html>. This page tracks how each condition is met and what
a maintainer still has to do. (Alternatives: Azure Artifact Signing, about 10 USD/month, shows your own legal
name; a self-signed certificate only helps on machines that trust it.)

## Conditions and status

| SignPath condition | How fairyfly meets it | Status |
|---|---|---|
| OSI-approved licence, no commercial dual licensing | [MIT `LICENSE`](../LICENSE) | done |
| No proprietary components (system libraries excepted) | [THIRD_PARTY_NOTICES.md](../THIRD_PARTY_NOTICES.md): BSD-3, MIT, PNG, CeCILL-C, BSL (tests only) | done |
| Only binaries built from its own source, built verifiably | [`release.yml`](../.github/workflows/release.yml): GitHub-hosted runner, tag checkout, unit tests, SignPath origin verification via the GitHub artifact | done |
| Public source repository, team owns it | `DataZooDE/fairyfly` is public | done |
| Product name = project name, product version consistent | `src/version_info.rc.in`: `ProductName fairyfly`, `ProductVersion` = `fairyfly --version`; checked in `build.yml` and `release.yml` | done |
| "Code signing policy" section with attribution, roles, privacy statement | README, section "Code signing policy" (exact attribution and privacy wording) | done |
| Functionality described on the download page | README, section "Download" | done |
| Roles Author / Reviewer / Approver | listed in the README; `.github/CODEOWNERS` makes the maintainer the required reviewer of CI, build and signing files | done (add team members when they join) |
| Build scripts and CI configuration are code-reviewed | `CODEOWNERS` plus branch protection on `main` (require a pull request and a code-owner review) | **maintainer: enable branch protection** |
| Multi-factor authentication for the repository and for SignPath | GitHub: require 2FA for the organization (Settings > Authentication security); SignPath: enable MFA for every user | **maintainer** |
| Project released in the form to be signed | publish a first release (an unsigned one is fine): `git tag v2026.09.30 && git push origin v2026.09.30` (the tag must equal `fairyfly --version`) | **maintainer** |
| Verifiable reputation (executables) | judged by SignPath (stars, users, history); not controllable from the code | open |

## Applying

1. Make sure the table above has no open maintainer items, then publish the first release (below).
2. Apply at <https://signpath.org/apply>: project name `fairyfly`, repository URL, release URL, licence MIT,
   a description (SAP GUI automation CLI and MCP server for the user's own SAP session), the roles above.
3. After approval SignPath issues an organization id, the project slug, the signing policy slug and an API token.
   Create in the GitHub repository: secret `SIGNPATH_API_TOKEN`, variable `SIGNPATH_ORGANIZATION_ID`. The workflow
   uses the project slug `fairyfly` and the signing policy slug `release-signing`: use these slugs when SignPath
   asks, or change them in `release.yml`.
4. In SignPath create the artifact configuration for the GitHub artifact (a zip with `fairyfly.exe`):

   ```xml
   <artifact-configuration xmlns="http://signpath.io/artifact-configuration/v1">
     <zip-file>
       <pe-file path="fairyfly.exe">
         <authenticode-sign/>
       </pe-file>
     </zip-file>
   </artifact-configuration>
   ```

   and set the metadata restrictions of the signing policy: `ProductName` must be `fairyfly` and
   `ProductVersion` must match the release (the workflow already checks both against the tag).
5. Restrict the signing policy to the origin `DataZooDE/fairyfly`, workflow `release.yml`, tag builds.

The workflow was written from the SignPath GitHub action documentation but could not be run before approval: the
exact input names and the artifact configuration may need small corrections when SignPath sends the onboarding data.

## Releasing

1. Bump the calendar version in `src/include/version.h`, `CMakeLists.txt` (`project(... VERSION Y.M.D)`) and
   `vcpkg.json`, update `CHANGELOG.md`, merge through a reviewed pull request.
2. `git tag vYYYY.MM.DD && git push origin vYYYY.MM.DD`. `release.yml` builds, tests, checks that the tag equals the
   product version, signs through SignPath when `SIGNPATH_API_TOKEN` exists (the approver confirms the request in
   SignPath) and creates the GitHub release with `fairyfly.exe` and `SHA256SUMS`. Without the secret the release is unsigned.
3. Verify a download: `signtool verify /pa /v fairyfly.exe` and `Get-FileHash fairyfly.exe` against `SHA256SUMS`.

## Notes

- The signature is created for the build artifact only; rebuilding locally produces an unsigned binary.
- SmartScreen reputation of a newly signed file still builds up over downloads.
- `SignPath Foundation` is the publisher name in the UAC prompt: that is how the free program works.
