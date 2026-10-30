# fairyfly as an MSIX package (Microsoft Store)

Status: the package is built. The build option, the manifest with fairyfly's Partner Center identity, the logos and
the packaging step are in the repository, and CI (`build-windows-msix`) packs an unsigned MSIX for the Store and a
test-signed copy for sideloading, which it installs and removes once. Every release attaches the unsigned MSIX
([Store submission](#store-submission)). The VM sideload test and the Store submission are
not done yet (see [What is left](#what-is-left)). The per-user MSI ([packaging/fairyfly.wxs](../packaging/fairyfly.wxs))
is unchanged and stays the default.

## Identity

| Partner Center | Value | Where |
|---|---|---|
| Package/Identity/Name | `DataZoo.fairyfly` | CMake `FAIRYFLY_MSIX_IDENTITY_NAME` |
| Package/Identity/Publisher | `CN=05AB657A-5AC7-43BC-A7F1-92AAD620BCCC` | CMake `FAIRYFLY_MSIX_PUBLISHER` |
| Package/Properties/PublisherDisplayName | `DataZoo` | CMake `FAIRYFLY_MSIX_PUBLISHER_DISPLAY_NAME` |
| Package family name | `DataZoo.fairyfly_73q587949tsjt` | follows from Name and Publisher |
| Store ID | `9N5TSPSX6Q0V` | the listing, `ms-windows-store://pdp/?productid=9N5TSPSX6Q0V` |

These values are public (every installed package's full name contains them), not secrets. They are the defaults of the
three cache variables ([cmake/msix_manifest.cmake](../cmake/msix_manifest.cmake), `FAIRYFLY_MSIX_DEFAULT_*`); `-D`
still overrides them, for example for a private test identity.
[packaging/msix/tests/Test-AppxManifest.ps1](../packaging/msix/tests/Test-AppxManifest.ps1) checks that the default
manifest carries exactly these values and that the package family name computed from them is the one Partner Center
shows.

## Build option `FAIRYFLY_PACKAGE`

| Value | Installer | Updates | Logon start of the link tray |
|---|---|---|---|
| `msi` (default) | per-user MSI, winget | `fairyfly update check\|apply` from the GitHub releases, and the MCP tray's background check | HKCU Run value `fairyfly-link` ("Start with Windows" in the tray) |
| `msix` | Microsoft Store package | the Store. `fairyfly update check\|apply` answer `UPDATE_MANAGED_BY_STORE` and ask nobody. The trays have no "Check for updates" item and no background check | the package's `StartupTask` (Settings > Apps > Startup). No Run value. `--install-autostart`/`--remove-autostart` answer `TRAY_AUTOSTART_PACKAGED` |

~~~powershell
cmake -B build-msix -G "Visual Studio 17 2022" -DFAIRYFLY_PACKAGE=msix `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build-msix --config Release --target fairyfly unit_tests
~~~

Configuring renders `build-msix/msix/AppxManifest.xml` from
[packaging/msix/AppxManifest.xml.in](../packaging/msix/AppxManifest.xml.in) through
[cmake/msix_manifest.cmake](../cmake/msix_manifest.cmake) with the identity above. The code reads the option through `include/packaging.h` (`FAIRYFLY_PACKAGE_MSIX` on `fairyfly_core`).

An `update` call in an MSIX build answers (the `suggestions` appear with verbose errors, as for every command):

~~~json
{"status": "error", "error": {"code": "UPDATE_MANAGED_BY_STORE",
  "message": "this fairyfly was installed from the Microsoft Store, which updates it; fairyfly does not replace a Store installation itself",
  "suggestions": ["Update fairyfly in the Microsoft Store app (Library > Get updates)", "..."],
  "update_channel": "microsoft-store", "current": "2026.10.27"}}
~~~

The package version is the calendar version as `YYYY.MM.(DD*100+N).0`, for example 2026.10.24 becomes 2026.10.2400.0
and 2026.10.24.1 becomes 2026.10.2401.0. MSIX needs four numbers from 0 to 65535, and the Store requires the fourth to be 0.
[packaging/msix_version.ps1](../packaging/msix_version.ps1) prints it, and `fairyfly_msix_version` computes it for the
manifest. [packaging/tests/Test-VersionScripts.ps1](../packaging/tests/Test-VersionScripts.ps1) checks that both give
the same result and that each version is higher than the one before.

### The manifest template

- **Tray** (`fairyfly-tray.exe`): the Start-menu tile. Without arguments it starts `fairyfly link run --tray`, like the
  MSI's shortcut. Its `desktop:StartupTask` (`fairyflyLinkTray`) replaces the Run value. It also declares the
  approval toast's COM activator (`{DAAF1065-…}`, `-Embedding`) as `com:ExeServer` + `desktop:ToastNotificationActivation`.
  Its AUMID `DataZoo.fairyfly_73q587949tsjt!Tray` is the approval toasts' (see [Approval notifications](#approval-notifications)).
- **Cli** (`fairyfly.exe`, `AppListEntry="none"`, console subsystem): `uap5:AppExecutionAlias` `fairyfly.exe`. This puts
  it on every terminal's PATH through `%LOCALAPPDATA%\Microsoft\WindowsApps`, so the MSI's user-PATH entry is not needed.
- `runFullTrust` only. No `unvirtualizedResources` and no `virtualization:ExcludedDirectories`: fairyfly's data is in
  `%USERPROFILE%\.fairyfly`, outside AppData (see [the decision below](#decision-the-data-directory-outside-appdata)).
- `Assets\Square44x44Logo.png` (44x44), `Square150x150Logo.png` (150x150), `Wide310x150Logo.png` (310x150, the tray's
  `DefaultTile`) and `StoreLogo.png` (50x50) are [packaging/msix/Assets](../packaging/msix/Assets): the app icon's look
  (the fairyfly mark on the light tile), drawn from the logo geometry by
  [assets/generate_icons.py](../assets/generate_icons.py) (`python assets/generate_icons.py`, Pillow). No scale
  variants: without a `resources.pri` Windows uses these files as they are and scales them.

## Packaging (CI)

`build-windows-msix` in [.github/workflows/build.yml](../.github/workflows/build.yml) does, after the msix build and its
unit tests, on the same runner:

1. **Payload = the MSI's.** [packaging/msix/New-MsixLayout.ps1](../packaging/msix/New-MsixLayout.ps1) stages
   `build-msix/layout`: every `<File>` of [packaging/fairyfly.wxs](../packaging/fairyfly.wxs) by its installed name
   (today `fairyfly.exe` and `fairyfly-tray.exe`, both statically linked), `AppxManifest.xml` and the `Assets\` images
   the manifest references. There is no second file list: a file added to the wxs is packaged in the MSIX too. Staging
   fails if the build lacks a file the MSI ships, if the manifest starts an `Executable` the MSI does not ship (the two
   have diverged) or if a referenced image is missing.
   [packaging/msix/tests/Test-MsixLayout.ps1](../packaging/msix/tests/Test-MsixLayout.ps1) (job `msix-manifest`, Linux)
   checks these refusals, the exact layout, and that `packaging/msix/Assets` holds exactly the referenced images in their
   sizes. CI also checks with `dumpbin /dependents` that neither exe imports a runtime DLL (`vcruntime*`, `msvcp*`, ...)
   that neither installer carries.
2. **`MakeAppx pack`** (Windows SDK on the runner) builds `fairyfly-<msixversion>.msix`, for example
   `fairyfly-2026.10.2800.0.msix`. Packing validates the manifest against the AppxManifest schema and fails on any
   error. An unpack round trip checks that the package holds exactly the layout. Steps 1 and 2 are
   [packaging/msix/New-MsixPackage.ps1](../packaging/msix/New-MsixPackage.ps1), which the release runs too.
   **Release dry run:** the job then runs the release's MSIX version check
   ([packaging/check_msix_version.ps1](../packaging/check_msix_version.ps1), see
   [Store submission](#store-submission)) with the tag `v<fairyfly --version>`: the version mapping and the
   unsigned package must pass; "above every published release" is only a warning there (a branch cut before the
   latest release is fine), the release enforces it. The workflow's token is read-only.
3. **Test-signed copy.** A self-signed code-signing certificate is made fresh in each run with Subject exactly the
   manifest's Publisher (`CN=05AB657A-5AC7-43BC-A7F1-92AAD620BCCC`, which Windows requires for a sideload). `signtool`
   signs a copy, `fairyfly-<msixversion>-testsigned.msix`; the public certificate is exported as
   `fairyfly-<msixversion>-testsigned.cer` and the private key is deleted from the runner. No secret is involved.
4. **Install check.** The runner imports the `.cer` into `LocalMachine\TrustedPeople`, installs the test-signed package
   with `Add-AppxPackage`, checks the package family name `DataZoo.fairyfly_73q587949tsjt` and the version, runs
   `fairyfly.exe --version` and `update check` through the execution alias, checks that `Get-StartApps` lists the
   tray's AUMID `DataZoo.fairyfly_73q587949tsjt!Tray` and that `fairyfly doctor` through the alias reports
   `Approval notifications as DataZoo.fairyfly_73q587949tsjt!Tray`, and removes the package again. A toast itself
   cannot be shown or clicked on a runner (no interactive desktop): that is the VM check below.

Artifacts of each run (Actions > the run > Artifacts, or `gh run download <run id> -R DataZooDE/fairyfly -n <name>`):

| Artifact | Content | Use |
|---|---|---|
| `fairyfly-msix` | `fairyfly-<msixversion>.msix`, **unsigned** | the Store submission (the Store signs it) |
| `fairyfly-msix-testsigned` | `fairyfly-<msixversion>-testsigned.msix` + `fairyfly-<msixversion>-testsigned.cer` | sideload tests on a test machine only |

The test certificate is **only for sideload tests**. It is not DataZoo's code-signing certificate, it is different in
every run, nobody holds its private key after the run, and a machine that trusts it trusts nothing else. Never
distribute the test-signed package or ask users to import the `.cer`.

## Sideload test on a test machine (VM)

Windows 10 1809 (17763) or later, x64, with sideloading allowed (the default on Windows 10 2004+ and Windows 11; else
Settings > For developers > Install apps from any source / Sideload apps). An MSI installation of fairyfly may stay;
for a clean test uninstall it first.

1. Download `fairyfly-msix-testsigned` from the CI run and unpack it.
2. In an **elevated** PowerShell, trust the run's test certificate for packages:
   `Import-Certificate -FilePath .\fairyfly-<msixversion>-testsigned.cer -CertStoreLocation Cert:\LocalMachine\TrustedPeople`
   (or double-click the `.cer` > Install Certificate > Local Machine > Trusted People).
3. As the normal user: `Add-AppxPackage .\fairyfly-<msixversion>-testsigned.msix` (or double-click the `.msix`).
4. Check:
   - Start menu: "fairyfly" starts the link tray; there is no console window and no "Check for updates" item.
   - Settings > Apps > Startup lists "fairyfly cloud link"; after a logoff/logon the link tray is running.
   - A **new** terminal: `fairyfly --version` and `fairyfly update check` (`UPDATE_MANAGED_BY_STORE`) run through the
     alias; `Get-Command fairyfly` shows `%LOCALAPPDATA%\Microsoft\WindowsApps\fairyfly.exe`.
   - An MCP client (stdio, `"command": "fairyfly"`) starts `fairyfly mcp`.
   - `fairyfly doctor` (new terminal) reports `approval_notifications` pass, `Approval notifications as
     DataZoo.fairyfly_73q587949tsjt!Tray`.
   - The approval toast, with the link paired and "Allow changes in SAP" **off** in the link tray: from the connected
     assistant, call a write tool (for example `gui_key_send` Enter in an open SAP session). A toast "<assistant> wants
     to change SAP" appears with Approve / Deny / Details. Click **Approve**: the change runs and the assistant gets the
     result. Again with **Deny**: nothing changes and the assistant gets the denial. Again, let the banner go to the
     notification centre and click Approve there: it still reaches the tray. **Details** opens the approval window.
     `%USERPROFILE%\.fairyfly\logs\link.log` shows `approval: notifications as DataZoo.fairyfly_73q587949tsjt!Tray`
     and no `CoRegisterClassObject` warning; Settings > System > Notifications lists "fairyfly".
   - `reg query HKCU\Software\Classes\AppUserModelId\DataZoo.fairyfly` finds nothing (from a normal terminal, with no
     MSI installed): the Store build registers nothing in HKCU.
   - `fairyfly mcp setup` works (Windows 10 and 11, see the table).
   - `%USERPROFILE%\.fairyfly\audit` and `logs` show up in Explorer (Windows 10 and 11) and stay after removal.
5. Remove: `Get-AppxPackage DataZoo.fairyfly | Remove-AppxPackage`, and the certificate from
   `Cert:\LocalMachine\TrustedPeople` (`Get-ChildItem Cert:\LocalMachine\TrustedPeople | Where-Object Subject -eq
   'CN=05AB657A-5AC7-43BC-A7F1-92AAD620BCCC' | Remove-Item`).

## Store submission

**What to upload.** Each GitHub release has the asset `fairyfly-<msixversion>.msix`, for example
`fairyfly-2026.10.2900.0.msix` for the tag `v2026.10.29`, next to `fairyfly.msi` and listed in `SHA256SUMS`. That
file is the Store package: Partner Center > fairyfly (9N5TSPSX6Q0V) > a new submission > Packages: upload it as it is.
It is **unsigned** on purpose: the Store checks that its identity matches the reserved one and signs it with the
Store's certificate. It is not the MSI's Authenticode signature (Artifact Signing signs `fairyfly.exe` and
`fairyfly.msi` only), and it does not install by double-click. Never upload or publish a test-signed package or a
test `.cer` (CI artifact `fairyfly-msix-testsigned`): a release never contains them. Before uploading, compare
`Get-FileHash fairyfly-<msixversion>.msix` with `SHA256SUMS`.

**Built by the release.** The job `build-msix` of [.github/workflows/release.yml](../.github/workflows/release.yml)
builds the msix configuration from the tagged commit (the same ref guard and `github.sha` checkout as the MSI build),
runs the unit tests, checks `fairyfly --version` against the tag (`check_release_tag.ps1`), packs with
[packaging/msix/New-MsixPackage.ps1](../packaging/msix/New-MsixPackage.ps1) and checks that the package is not signed.
It has no environment, no Azure login and no secret. The release is only published when this job succeeds, and
the release job checks the attached file against the SHA-256 that `build-msix` took of the checked package.

**Version check.** The Store refuses a package whose version is not higher than the last submitted one, so
[packaging/check_msix_version.ps1](../packaging/check_msix_version.ps1) fails the release unless the packed
`Package/Identity/Version` is exactly [packaging/msix_version.ps1](../packaging/msix_version.ps1) of the tag,
`vYYYY.MM.DD[.N]` -> `YYYY.MM.(DD*100+N).0` (`v2026.10.29` -> `2026.10.2900.0`, `v2026.10.29.1` -> `2026.10.2901.0`,
`v2026.11.01` -> `2026.11.100.0`), and strictly above the MSIX version of every other published release (computed
from their tags the same way, so releases from before the MSIX count too; non-calendar tags are ignored). A tag
below an existing release, or a same-day `.N` below one already released, fails before anything is published. Two
tags pushed at the same time are not checked against each other (neither release exists yet); release one at a time.
Submit the releases in order: uploading an older release's MSIX after a newer one was submitted is refused by the
Store.

The submission also needs the justification for the restricted capability `runFullTrust` and the listing.

## What fairyfly writes at run time, and what MSIX does with it

The MSIX rules for a packaged full-trust app (Windows 10 1903 and later,
[behind the scenes](https://learn.microsoft.com/en-us/windows/msix/desktop/desktop-to-uwp-behind-the-scenes),
[flexible virtualization](https://learn.microsoft.com/en-us/windows/msix/desktop/flexible-virtualization)):

- The install folder (`C:\Program Files\WindowsApps\<package>`) is read-only.
- New files and folders under `%LOCALAPPDATA%`/`%APPDATA%` go to a private per-package location
  (`%LOCALAPPDATA%\Packages\<family>\LocalCache\…`). Only the package's own processes see them, and they are deleted
  when the package is uninstalled. A file that already exists outside that location (for example from an earlier MSI
  install) is modified in place.
- Every HKCU write goes to a private per-package hive, copy on write.
- Other locations (the user profile outside AppData, HKLM with elevation, Credential Manager, http.sys, the certificate
  store) are not virtualized.

All fairyfly processes started through the package (the tray, its children, `fairyfly` from a terminal through the
alias, and an MCP client that starts `fairyfly mcp` through the alias) share the package view. A process that runs
without the package identity does not see that view: Explorer, Notepad, an admin's script, and most likely the elevated
`mcp setup --apply-plan` child, which `runas` starts by path (to check on the VM).

| What | Path | Writer | Under MSIX | What fairyfly must do |
|---|---|---|---|---|
| Self-update (`fairyfly.exe.new`/`.old`, rename of the running exe) | install folder | `src/update/updater.cpp` `apply()` | impossible: read-only | **done**: the msix build does not update itself (`UPDATE_MANAGED_BY_STORE`, also in `apply()`). The tray has no update item and no background check |
| Link tray autostart | `HKCU\…\CurrentVersion\Run` `fairyfly-link` (and `fairyfly-mcp` for `mcp --tray --install-autostart`) | `src/tray/tray_win32.cpp` `Win32RunKey`, `settle_autostart`, `decide_tray_entry` | **breaks**: lands in the private hive, Explorer never sees it, so no autostart | **done**: no Run value in the msix build. `StartupTask` in the manifest. `--install-autostart` → `TRAY_AUTOSTART_PACKAGED`. Open: the MCP tray has no StartupTask (one more Application + task if it is wanted) |
| Tray settings (`link_allow_changes`, `link-autostart-defaulted`) | `HKCU\Software\DataZoo\fairyfly\tray` | `Win32TraySettings` | private hive. Only fairyfly reads it | nothing. It starts empty after a move from the MSI (the standing approval is asked again) |
| Toast activator / AUMID registration | `HKCU\Software\Classes\AppUserModelId\DataZoo.fairyfly`, `…\CLSID\{DAAF1065-…}\LocalServer32` | `src/link/desktop_approval_win32.cpp` | **breaks**: private hive, so the notification centre cannot activate the approval toast. A packaged process also has the package's AUMID | **done**: the manifest declares the COM server + toast activation; the msix build writes nothing to HKCU, does not call `SetCurrentProcessExplicitAppUserModelID` and shows its toasts as `<PackageFamilyName>!Tray` ([Approval notifications](#approval-notifications)). Check the toast on the VM |
| Audit trail | `%USERPROFILE%\.fairyfly\audit\YYYY-MM.jsonl` (or `FAIRYFLY_AUDIT_FILE`) | `src/audit_log.cpp` | not virtualized (user profile outside AppData): Explorer and admins see it, it stays after an uninstall | **done**: the data directory moved out of AppData ([DATA_DIR.md](DATA_DIR.md)) |
| Logs | `%USERPROFILE%\.fairyfly\logs\mcp.log`, `link.log` | `route_logging_to_file` | not virtualized | done, as above |
| MCP config | `%USERPROFILE%\.fairyfly\mcp.yaml` | `mcp config init`, `mcp setup` | not virtualized | done, as above |
| `mcp setup` plan/result, manifest, cert export | `%USERPROFILE%\.fairyfly\run\plan-*.json`, `result-*.json`, `mcp-setup.json`, `fairyfly-mcp-<host>.cer` | `src/setup/elevator_win.cpp`, `setup_service.cpp`, `cert_store_win.cpp` | not virtualized: the elevated child (started by path, most likely without package identity) finds the plan on Windows 10 too | done, as above. http.sys, the certificate store and the firewall are machine-wide and not virtualized. Check `mcp setup` on the VM |
| Link pairing state | `%USERPROFILE%\.fairyfly\link\state.json` (+ `.status.json`, `.paused`, DPAPI-protected) | `src/link/link_runner.cpp`, `link_control.cpp` | not virtualized: survives a reinstall, as with the MSI | done, as above |
| Connection cache | `%USERPROFILE%\.fairyfly\sessions` (no temp fallback) | `src/connection_manager.cpp` | not virtualized | nothing |
| Up to 2026.10.28: all of the above in `%LOCALAPPDATA%\fairyfly` | `%LOCALAPPDATA%\fairyfly`, and for a Store fairyfly on Windows 10 `%LOCALAPPDATA%\Packages\DataZoo.fairyfly_73q587949tsjt\LocalCache\Local\fairyfly` | `src/config/data_dir.cpp` (read once) | the package copy is reachable by path from the packaged process | **done**: the first start moves both to `%USERPROFILE%\.fairyfly` ([DATA_DIR.md](DATA_DIR.md#the-move-from-localappdatafairyfly)) |
| MCP tokens, SAP credentials | Windows Credential Manager (`fairyfly-mcp:*`, `fairyfly:*`) | `src/auth/`, `src/credential_store.cpp` | not virtualized | nothing |
| User-chosen files (`screen capture --output`, `link pair --qr-png`, `mcp cert export --out`) | anywhere | the commands | outside AppData: real. Inside AppData: private | nothing. Document that paths under AppData stay inside the package |
| Claude Desktop MCP config | `%APPDATA%\Claude\claude_desktop_config.json` (an MSIX-packaged Claude Desktop keeps its own in `%LOCALAPPDATA%\Packages\Claude_…\LocalCache\Roaming\Claude\`) | **nobody in fairyfly**: `mcp client-config` (`src/config/client_config.cpp`) only prints snippets, and the user pastes them | n/a | **nothing to fix, keep it that way.** If fairyfly ever writes it: a new file would land in fairyfly's private store, so Claude would not see it. An existing one would be changed in place. A packaged Claude's file cannot be reached at all. Edit it through Claude's own UI (Settings > Developer > Edit config) instead. The stdio snippet's `"command": "fairyfly"` works through the execution alias. Never print the `WindowsApps\…` path: it changes with every update |
| MSI-only state | `HKCU\Software\DataZoo\fairyfly` `installed`/`shortcut`, user PATH, Start-menu `.lnk` | `packaging/fairyfly.wxs` | not used | the manifest covers these (tile, alias) |

### Cross-app visibility: what breaks

1. **Run-key autostart** (fixed in this build option: StartupTask).
2. **Approval toast activation** through the HKCU COM/AUMID registration (fixed: the manifest declares it, the code uses
   the package's AUMID; check the click on the VM).
3. **`mcp setup`'s plan hand-over** to the elevated child (fixed: the plan is in `%USERPROFILE%\.fairyfly\run`; check on
   Windows 10 and 11).
4. **Audit trail, logs and `mcp.yaml` seen from outside** (Explorer, editors, admins), and the audit trail **being deleted on
   uninstall** (fixed: `%USERPROFILE%\.fairyfly` is not virtualized).
5. The Claude Desktop config is **not** affected, because fairyfly does not write it.

### Approval notifications

The cloud link asks at the desktop before a change in SAP: a toast with Approve / Deny / Details
([src/link/desktop_approval_win32.cpp](../src/link/desktop_approval_win32.cpp)). Which AppUserModelID (AUMID) it uses
and what it registers is decided in [src/include/link/toast_identity.h](../src/include/link/toast_identity.h):

| Build | AUMID of the toasts | Registration | Activator's server |
|---|---|---|---|
| `msi` | `DataZoo.fairyfly` (the Start-menu shortcut's) | the link writes `HKCU\Software\Classes\AppUserModelId\DataZoo.fairyfly` (`CustomActivator`) and `…\CLSID\{DAAF1065-…}\LocalServer32` = `fairyfly-tray.exe`, and sets the AUMID on its process | COM appends `-Embedding` |
| `msix` | `<PackageFamilyName>!Tray`, i.e. `DataZoo.fairyfly_73q587949tsjt!Tray`: the family part of `GetCurrentApplicationUserModelId` (`GetCurrentPackageFamilyName` as fallback) | none at run time: the manifest's `com:ExeServer` and `desktop:ToastNotificationActivation` on the `Tray` application. No HKCU value, no `SetCurrentProcessExplicitAppUserModelID` | the manifest's `Arguments="-Embedding"` |
| `msix` run outside its package | none: no toast, the approval window asks; `fairyfly doctor` warns | none | - |

`fairyfly link run` through the execution alias runs as the `Cli` application; its toasts are still the `Tray`
application's, which declares the activator. In both builds the running link registers the activator's class object
(`CoRegisterClassObject`, in a multithreaded apartment kept for the process) as it starts, so a click on the toast or
in the notification centre reaches the running tray. When no tray runs, COM starts `fairyfly-tray.exe -Embedding`,
which starts the link tray like the Start menu; once paired, that link registers the class and receives the click and
drops it, because the question belonged to the process that ended (and was denied with it).
`fairyfly doctor` reports the AUMID as the check `approval_notifications`.
[packaging/msix/tests/Test-AppxManifest.ps1](../packaging/msix/tests/Test-AppxManifest.ps1) checks that the manifest's
CLSID, server argument and application id are the code's
([src/include/tray/tray_contract.h](../src/include/tray/tray_contract.h)).

### Decision: the data directory outside AppData

fairyfly keeps its data in `%USERPROFILE%\.fairyfly` ([DATA_DIR.md](DATA_DIR.md)), for MSI and MSIX alike, on every
Windows version. The manifest has no `ExcludedDirectories` and no `unvirtualizedResources`.

Up to 2026.10.28 the data was in `%LOCALAPPDATA%\fairyfly`, and the manifest excluded that folder from virtualization.
That is not enough: verified live on Windows 10 (19045) on 2026-10-29 with `Invoke-CommandInDesktopPackage`, a new folder
under `%LOCALAPPDATA%` written by the package (`%LOCALAPPDATA%\ffyprobefresh\new.txt`) landed only in
`%LOCALAPPDATA%\Packages\DataZoo.fairyfly_73q587949tsjt\LocalCache\Local\…`; writes into an already existing
`%LOCALAPPDATA%\fairyfly` went to the real folder. `ExcludedDirectories` only works on Windows 11. So a fresh Store
customer on Windows 10 would have had an audit trail, logs, `mcp.yaml` and link state that no other program sees and that
an uninstall deletes. The user profile outside AppData is not virtualized on any Windows version.

- Removing `unvirtualizedResources` leaves `runFullTrust` as the only restricted capability: one justification less in
  the Store submission, and no risk that Partner Center refuses it.
- Do **not** add `desktop6:FileSystemWriteVirtualization`/`RegistryWriteVirtualization` `disabled` either. The registry
  needs no exclusion once the Run value and the toast registration are handled as above.
- Not `Documents`: it is often redirected to OneDrive (synced, sometimes on a network share), which is wrong for an
  audit trail and a DPAPI-protected device key. A dot folder in the profile is where developer tools keep per-user
  state on Windows too (`.ssh`, `.aws`, `.docker`, `.vscode`).

## What is left

1. **Identity**: done (Partner Center values are the CMake defaults; [Identity](#identity)).
2. **Packaging**: done in CI ([Packaging (CI)](#packaging-ci)) and in the release: every release attaches the
   unsigned `fairyfly-<msixversion>.msix` ([Store submission](#store-submission)).
3. **VM sideload test** ([steps](#sideload-test-on-a-test-machine-vm)) (`Add-AppxPackage`): the StartupTask starts the link tray at logon. `fairyfly` from a fresh terminal
   and from an MCP client (stdio) runs through the alias. The approval toast's Approve/Deny buttons activate the tray (the code is done; the click needs the VM).
   `mcp setup` works, on Windows 10 and 11. The audit trail and logs show up in Explorer under `%USERPROFILE%\.fairyfly`
   (also on Windows 10), and stay after `Remove-AppxPackage`. An earlier Store install's data in the package copy is
   moved there at the first start. `update check` answers `UPDATE_MANAGED_BY_STORE`.
4. **Code**: the toast AUMID/registration for packaged processes is done ([Approval notifications](#approval-notifications)).
   Optionally a StartupTask for the MCP tray. Optionally switch the StartupTask from the tray menu (WinRT `StartupTask`).
5. **Store submission**: upload a release's `fairyfly-<msixversion>.msix` (signed by the Store), the restricted
   capability justification (`runFullTrust`), and the listing.
