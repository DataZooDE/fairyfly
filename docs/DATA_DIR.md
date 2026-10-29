# The data directory

fairyfly keeps everything it stores per user in one folder:

~~~text
%USERPROFILE%\.fairyfly          e.g. C:\Users\ada\.fairyfly
~~~

It is the same for the MSI, the Microsoft Store (MSIX) package and the single `fairyfly.exe`, on Windows 10 and 11.
Uninstalling fairyfly never deletes it.

| What | Where in it | Own override |
|---|---|---|
| Audit trail | `audit\YYYY-MM.jsonl` | `FAIRYFLY_AUDIT_FILE`, `audit.file` in `mcp.yaml` |
| Logs of the trays | `logs\mcp.log`, `logs\link.log` | |
| MCP server configuration | `mcp.yaml` | `-c`, `FAIRYFLY_MCP_CONFIG` |
| Cloud link pairing | `link\state.json` (device key DPAPI-protected), `state.json.status.json`, `state.json.paused` | `link ... --state` |
| Saved connections | `sessions\*.con` | `FAIRYFLY_CACHE_DIR` |
| `mcp setup` | `mcp-setup.json`, `run\plan-*.json`, `run\result-*.json`, `fairyfly-mcp-<host>.cer` | `mcp cert export --out` |
| Marker | `.data-dir.json` (when the folder was created and what was moved into it) | |

Not in it: SAP credentials and MCP access tokens (Windows Credential Manager, `fairyfly:*` and `fairyfly-mcp:*`) and
the trays' settings (`HKCU\Software\DataZoo\fairyfly\tray`).

`FAIRYFLY_DATA_DIR` moves the whole folder (tests, a shared machine with a different layout). With it set, nothing is
moved from the old location. Without `USERPROFILE`, `HOME\.fairyfly` is used, and without either `%TEMP%\fairyfly`.
In code, `config::Environment::data_dir()` is the one root every path derives from (`src/include/config/environment.h`).

## Why not %LOCALAPPDATA%

Up to 2026.10.28 the folder was `%LOCALAPPDATA%\fairyfly`. A Microsoft Store app on Windows 10 cannot keep shared data
there: verified live on 2026-10-29 on Windows 10 (19045), a write by the Store package into a **new** folder under
`%LOCALAPPDATA%` lands only in the package's private copy
(`%LOCALAPPDATA%\Packages\DataZoo.fairyfly_73q587949tsjt\LocalCache\Local\…`), which other programs (Explorer, editors,
the elevated `mcp setup` step, an admin collecting the audit trail) do not see and which Windows deletes when the
package is removed. The manifest setting that would prevent that (`ExcludedDirectories`) only works on Windows 11. The
user profile outside AppData is not virtualized on any Windows version, so one location works for every install.
`Documents` was not chosen because it is often redirected to OneDrive. [MSIX.md](MSIX.md) has the details.

The folder is not hidden, like `.ssh`, `.aws` or `.docker` on Windows: the audit trail is meant to be found (the trays'
*Open audit trail folder* opens it), and Explorer hides nothing that starts with a dot. Its permissions are inherited
from the profile folder: only the user, SYSTEM and the administrators have access, as under `%LOCALAPPDATA%` before.
fairyfly sets no permissions of its own, then and now.

## The move from %LOCALAPPDATA%\fairyfly

The first start of a fairyfly that has this change (any command, the tray, `fairyfly mcp`) moves the old data before it
opens anything in the folder (`config::migrate_process_data_dir`, `src/config/data_dir.cpp`):

1. When `%USERPROFILE%\.fairyfly\.data-dir.json` exists, there is nothing to do. This is every start after the first.
2. Otherwise it takes a per-user lock (the tray and the CLI often start together; the others wait up to 30 s, then go
   on with the new folder and print a warning) and checks again.
3. The old folders: `%LOCALAPPDATA%\fairyfly`, and for the Store package also its private copy
   `%LOCALAPPDATA%\Packages\<family>\LocalCache\Local\fairyfly` (a Store fairyfly on Windows 10 that ran before had its
   data there). A folder with the note `MOVED-TO-USERPROFILE-.fairyfly.txt` was moved already and is skipped.
4. No old folder: the new folder is created with its marker.
5. The new folder does not exist: the old folders are copied into `%USERPROFILE%\.fairyfly.migrating`, the marker is
   written, and that folder is renamed to `.fairyfly`. A leftover `.fairyfly.migrating` from an interrupted move is
   deleted first, so an interrupted move simply starts over. Then the old folders are merged once more, for records an
   older fairyfly appended while they were copied.
6. The new folder exists without the marker (made by hand, or by another program): the old folders are merged into it.
7. Each old folder gets the note `MOVED-TO-USERPROFILE-.fairyfly.txt`. fairyfly never deletes the old data; delete it
   yourself once you have checked the new folder.

Merging never overwrites anything. A file the new folder lacks is copied (to a temporary name, then renamed). An audit
trail file (`*.jsonl`) that exists on both sides gets the records it lacks appended: no record is lost and none is
duplicated, also when the same folder is seen twice (a Store process sees `%LOCALAPPDATA%\fairyfly` merged with its
private copy) or when a move is repeated. A last line without a line end (a record still being written) is not copied.
Any other file that differs keeps the copy found first (`%LOCALAPPDATA%\fairyfly` before the package copy).

When the new folder cannot be written, the old folders stay untouched, fairyfly prints a warning to stderr and tries
again at the next start. With a required audit trail (`--audit-required`, `FAIRYFLY_AUDIT=required`,
`audit.required: true`) the command then fails closed with `AUDIT_UNAVAILABLE`, naming the folder and the reason.

An older fairyfly that still runs (another installation) keeps writing to `%LOCALAPPDATA%\fairyfly`. Records it writes
after the move are not moved; update or remove it.

## Uninstall

The MSI's uninstall (`fairyfly-tray.exe --uninstall`) removes the trays' Run values and registry settings and the
notification registration, nothing else. Removing the Store package deletes only its private copies. In both cases
`%USERPROFILE%\.fairyfly` stays, like the audit trail it holds; delete it yourself if you no longer need it.
