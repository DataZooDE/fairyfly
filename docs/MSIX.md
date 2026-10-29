# fairyfly as an MSIX package (Microsoft Store)

Status: the package is built. The build option, the manifest with fairyfly's Partner Center identity, the logos and
the packaging step are in the repository, and CI (`build-windows-msix`) packs an unsigned MSIX for the Store and a
test-signed copy for sideloading, which it installs and removes once. The VM sideload test and the Store submission are
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
- **Cli** (`fairyfly.exe`, `AppListEntry="none"`, console subsystem): `uap5:AppExecutionAlias` `fairyfly.exe`. This puts
  it on every terminal's PATH through `%LOCALAPPDATA%\Microsoft\WindowsApps`, so the MSI's user-PATH entry is not needed.
- `runFullTrust`, and `unvirtualizedResources` with `virtualization:ExcludedDirectories` for
  `$(KnownFolder:LocalAppData)\fairyfly` (see the recommendation below).
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
   error. An unpack round trip checks that the package holds exactly the layout.
3. **Test-signed copy.** A self-signed code-signing certificate is made fresh in each run with Subject exactly the
   manifest's Publisher (`CN=05AB657A-5AC7-43BC-A7F1-92AAD620BCCC`, which Windows requires for a sideload). `signtool`
   signs a copy, `fairyfly-<msixversion>-testsigned.msix`; the public certificate is exported as
   `fairyfly-<msixversion>-testsigned.cer` and the private key is deleted from the runner. No secret is involved.
4. **Install check.** The runner imports the `.cer` into `LocalMachine\TrustedPeople`, installs the test-signed package
   with `Add-AppxPackage`, checks the package family name `DataZoo.fairyfly_73q587949tsjt` and the version, runs
   `fairyfly.exe --version` and `update check` through the execution alias and removes the package again.

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
   - The approval toast's Approve/Deny buttons reach the tray (open: needs the code change in the table below).
   - `fairyfly mcp setup` works (Windows 10 and 11, see the table).
   - `%LOCALAPPDATA%\fairyfly\audit` and `logs` show up in Explorer (Windows 11) and stay after removal.
5. Remove: `Get-AppxPackage DataZoo.fairyfly | Remove-AppxPackage`, and the certificate from
   `Cert:\LocalMachine\TrustedPeople` (`Get-ChildItem Cert:\LocalMachine\TrustedPeople | Where-Object Subject -eq
   'CN=05AB657A-5AC7-43BC-A7F1-92AAD620BCCC' | Remove-Item`).

## Store submission (later)

Partner Center > fairyfly (9N5TSPSX6Q0V) > a submission > Packages: upload the **unsigned** `fairyfly-<msixversion>.msix`
from the `fairyfly-msix` artifact, never the test-signed one. The Store checks that its identity matches the reserved
one and signs it with the Store's certificate; the version must be higher than the last submitted one (the calendar
version scheme above guarantees that). The submission also needs the justifications for the restricted capabilities
`runFullTrust` and `unvirtualizedResources` (see the recommendation below) and the listing.

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
| Toast activator / AUMID registration | `HKCU\Software\Classes\AppUserModelId\DataZoo.fairyfly`, `…\CLSID\{DAAF1065-…}\LocalServer32` | `src/link/desktop_approval_win32.cpp` | **breaks**: private hive, so the notification centre cannot activate the approval toast. A packaged process also has the package's AUMID | template declares the COM server + toast activation. **Open (code)**: in the msix build, skip the HKCU registration and `SetCurrentProcessExplicitAppUserModelID`, and create the notifier with the package AUMID (`<PackageFamilyName>!Tray`). Check the toast on the VM |
| Audit trail | `%LOCALAPPDATA%\fairyfly\audit\YYYY-MM.jsonl` (or `FAIRYFLY_AUDIT_FILE`) | `src/audit_log.cpp` | new folder → private. Invisible to Explorer and admins, and **deleted on uninstall** | exclude `%LOCALAPPDATA%\fairyfly` (template does this). Win10: the tray's "Open audit trail folder" shows the real, empty folder |
| Logs | `%LOCALAPPDATA%\fairyfly\logs\mcp.log`, `link.log` | `route_logging_to_file` | private | as above. "Open log" through Explorer/Notepad needs the exclusion |
| MCP config | `%LOCALAPPDATA%\fairyfly\mcp.yaml` | `mcp config init`, `mcp setup` | private | as above ("Open config file", editing it by hand) |
| `mcp setup` plan/result, manifest, cert export | `%LOCALAPPDATA%\fairyfly\run\plan-*.json`, `result-*.json`, `mcp-setup.json`, `fairyfly-mcp-<host>.cer` | `src/setup/elevator_win.cpp`, `setup_service.cpp`, `cert_store_win.cpp` | **breaks**: the unelevated packaged process writes the plan to the private store, and the elevated child started by path most likely has no package identity (check on the VM), so it cannot find the plan. Its result goes to the real folder (fairyfly can read it back through the read fallback) | the exclusion fixes it on Windows 11. On Windows 10 `mcp setup` needs a test, and probably a plan path outside AppData. http.sys, the certificate store and the firewall are machine-wide and not virtualized |
| Link pairing state | `%LOCALAPPDATA%\fairyfly\link\state.json` (+ `.status.json`, `.paused`, DPAPI-protected) | `src/link/link_runner.cpp`, `link_control.cpp` | private. Only fairyfly reads it | works without the exclusion. With the exclusion it survives a reinstall, as with the MSI. After a move from the MSI the existing file is used in place |
| Connection cache | `%LOCALAPPDATA%\fairyfly\sessions` (fallback `%TEMP%\fairyfly\sessions`) | `src/connection_manager.cpp` | private | nothing |
| MCP tokens, SAP credentials | Windows Credential Manager (`fairyfly-mcp:*`, `fairyfly:*`) | `src/auth/`, `src/credential_store.cpp` | not virtualized | nothing |
| User-chosen files (`screen capture --output`, `link pair --qr-png`, `mcp cert export --out`) | anywhere | the commands | outside AppData: real. Inside AppData: private | nothing. Document that paths under AppData stay inside the package |
| Claude Desktop MCP config | `%APPDATA%\Claude\claude_desktop_config.json` (an MSIX-packaged Claude Desktop keeps its own in `%LOCALAPPDATA%\Packages\Claude_…\LocalCache\Roaming\Claude\`) | **nobody in fairyfly**: `mcp client-config` (`src/config/client_config.cpp`) only prints snippets, and the user pastes them | n/a | **nothing to fix, keep it that way.** If fairyfly ever writes it: a new file would land in fairyfly's private store, so Claude would not see it. An existing one would be changed in place. A packaged Claude's file cannot be reached at all. Edit it through Claude's own UI (Settings > Developer > Edit config) instead. The stdio snippet's `"command": "fairyfly"` works through the execution alias. Never print the `WindowsApps\…` path: it changes with every update |
| MSI-only state | `HKCU\Software\DataZoo\fairyfly` `installed`/`shortcut`, user PATH, Start-menu `.lnk` | `packaging/fairyfly.wxs` | not used | the manifest covers these (tile, alias) |

### Cross-app visibility: what breaks

1. **Run-key autostart** (fixed in this build option: StartupTask).
2. **Approval toast activation** through the HKCU COM/AUMID registration (manifest part done, code change open).
3. **`mcp setup`'s plan hand-over** to the elevated child (needs the exclusion, and a test on Windows 10).
4. **Audit trail, logs and `mcp.yaml` seen from outside** (Explorer, editors, admins), and the audit trail **being deleted on
   uninstall** (needs the exclusion).
5. The Claude Desktop config is **not** affected, because fairyfly does not write it.

### Recommendation: `unvirtualizedResources` with `ExcludedDirectories`

Yes, for `%LOCALAPPDATA%\fairyfly` only, as the template declares: `rescap:Capability unvirtualizedResources` plus
`virtualization:FileSystemWriteVirtualization/ExcludedDirectories/$(KnownFolder:LocalAppData)\fairyfly`. The audit trail
is a record that must stay readable by the user and admins and must outlive the app (point 4). `mcp setup` needs it
(point 3). And it keeps one data folder for MSI and MSIX installs.

- Do **not** add `desktop6:FileSystemWriteVirtualization`/`RegistryWriteVirtualization` `disabled`. Those turn
  virtualization off for all of AppData/HKCU and apply on Windows 10 too. The registry needs no exclusion once the Run
  value and the toast registration are handled as above.
- `ExcludedDirectories` works on Windows 11 only. On Windows 10 (MinVersion 10.0.17763) the folder stays virtualized:
  points 3 and 4 remain there. Either raise MinVersion to Windows 11 (10.0.22000), or accept them and document them for
  Windows 10.
- `unvirtualizedResources` is a restricted capability. The Store submission needs a justification (audit trail and
  config shared with the user's tools and admins, kept on uninstall). If Partner Center refuses it, remove both entries.
  fairyfly still works then, with points 3 and 4 open.

## What is left

1. **Identity**: done (Partner Center values are the CMake defaults; [Identity](#identity)).
2. **Packaging**: done in CI ([Packaging (CI)](#packaging-ci)). Not in the release workflow yet: a release does not
   attach the MSIX.
3. **VM sideload test** ([steps](#sideload-test-on-a-test-machine-vm)) (`Add-AppxPackage`): the StartupTask starts the link tray at logon. `fairyfly` from a fresh terminal
   and from an MCP client (stdio) runs through the alias. The approval toast's Approve/Deny buttons activate the tray.
   `mcp setup` works, on Windows 10 and 11. The audit trail and logs show up in Explorer, and stay after
   `Remove-AppxPackage`. `update check` answers `UPDATE_MANAGED_BY_STORE`.
4. **Code**: the toast AUMID/registration change for packaged processes (table above). Optionally a StartupTask for the
   MCP tray. Optionally switch the StartupTask from the tray menu (WinRT `StartupTask`).
5. **Store submission**: signing by the Store, the restricted capability justifications (`runFullTrust`,
   `unvirtualizedResources`), and the listing.
