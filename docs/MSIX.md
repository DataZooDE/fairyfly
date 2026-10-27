# fairyfly as an MSIX package (Microsoft Store)

Status: preparation. The build option, the manifest template and this audit are in the repository. The Partner Center
identity, the packaging step (MakeAppx), signing and the Store submission are not done yet (see
[What is left](#what-is-left)). The per-user MSI ([packaging/fairyfly.wxs](../packaging/fairyfly.wxs)) is unchanged and
stays the default.

## Build option `FAIRYFLY_PACKAGE`

| Value | Installer | Updates | Logon start of the link tray |
|---|---|---|---|
| `msi` (default) | per-user MSI, winget | `fairyfly update check\|apply` from the GitHub releases, and the MCP tray's background check | HKCU Run value `fairyfly-link` ("Start with Windows" in the tray) |
| `msix` | Microsoft Store package | the Store. `fairyfly update check\|apply` answer `UPDATE_MANAGED_BY_STORE` and ask nobody. The trays have no "Check for updates" item and no background check | the package's `StartupTask` (Settings > Apps > Startup). No Run value. `--install-autostart`/`--remove-autostart` answer `TRAY_AUTOSTART_PACKAGED` |

~~~powershell
cmake -B build-msix -G "Visual Studio 17 2022" -DFAIRYFLY_PACKAGE=msix `
  -DFAIRYFLY_MSIX_IDENTITY_NAME=<Package/Identity/Name> `
  -DFAIRYFLY_MSIX_PUBLISHER="CN=<Package/Identity/Publisher>" `
  -DFAIRYFLY_MSIX_PUBLISHER_DISPLAY_NAME="<Package/Properties/PublisherDisplayName>" `
  -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build-msix --config Release --target fairyfly unit_tests
~~~

Configuring renders `build-msix/msix/AppxManifest.xml` from
[packaging/msix/AppxManifest.xml.in](../packaging/msix/AppxManifest.xml.in) through
[cmake/msix_manifest.cmake](../cmake/msix_manifest.cmake). Without the three identity values it uses `PLACEHOLDER`
values and warns. The code reads the option through `include/packaging.h` (`FAIRYFLY_PACKAGE_MSIX` on `fairyfly_core`).

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
- `Assets\StoreLogo.png`, `Square150x150Logo.png` and `Square44x44Logo.png` are referenced but not in the repository yet.

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

1. **Identity**: Package Name, Publisher (`CN=…`) and PublisherDisplayName from Partner Center into the three CMake
   cache variables (CI: repository variables). Check them against the Store listing.
2. **Packaging**: logo assets, a layout folder (`fairyfly.exe`, `fairyfly-tray.exe`, `AppxManifest.xml`, `Assets\`),
   `makeappx pack`, and a test certificate whose subject equals the Publisher.
3. **VM sideload test** (`Add-AppxPackage`): the StartupTask starts the link tray at logon. `fairyfly` from a fresh terminal
   and from an MCP client (stdio) runs through the alias. The approval toast's Approve/Deny buttons activate the tray.
   `mcp setup` works, on Windows 10 and 11. The audit trail and logs show up in Explorer, and stay after
   `Remove-AppxPackage`. `update check` answers `UPDATE_MANAGED_BY_STORE`.
4. **Code**: the toast AUMID/registration change for packaged processes (table above). Optionally a StartupTask for the
   MCP tray. Optionally switch the StartupTask from the tray menu (WinRT `StartupTask`).
5. **Store submission**: signing by the Store, the restricted capability justifications (`runFullTrust`,
   `unvirtualizedResources`), and the listing.
