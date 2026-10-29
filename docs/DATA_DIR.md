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
moved from the old location. A process started without `USERPROFILE` (a stripped environment) reads the profile from
Windows (the Profile known folder); elsewhere `$HOME/.fairyfly` is used. fairyfly never falls back to the temp folder
or the current directory: without a profile there is no data directory, the audit trail, the logs, saved connections
and the pairing are not written, `--audit-required` fails with `AUDIT_UNAVAILABLE` and `fairyfly doctor` reports
`data_dir` as failed (`DATA_DIR_UNAVAILABLE`).
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

The old folders are `%LOCALAPPDATA%\fairyfly` and, for the Store package, also its private copy
`%LOCALAPPDATA%\Packages\<family>\LocalCache\Local\fairyfly` (a Store fairyfly on Windows 10 that ran before had its
data there; only a packaged fairyfly can see it). An old folder is *pending* while it exists and has no note
`MOVED-TO-USERPROFILE-.fairyfly.txt`. The note is written only after the folder was merged completely.

1. When `%USERPROFILE%\.fairyfly\.data-dir.json` exists and no old folder is pending, there is nothing to do. This is
   every start after the first. When an MSI fairyfly moved `%LOCALAPPDATA%\fairyfly` first and the Store package's
   private copy is still pending, the Store fairyfly's first start merges it then (and the other way round).
2. Otherwise it takes a per-user lock (the tray and the CLI often start together; the others wait up to 30 s, then go
   on and print a warning) and checks again.
3. Nothing pending: the new folder is created with its marker.
4. The new folder does not exist: the pending folders are copied into `%USERPROFILE%\.fairyfly.migrating`, and that
   folder is renamed to `.fairyfly`. A leftover `.fairyfly.migrating` from an interrupted move is deleted first, so an
   interrupted move simply starts over. Then the old folders are merged once more, for records an older fairyfly
   appended while they were copied.
5. The new folder exists: the pending folders are merged into it.
6. The marker records each merged folder and every conflict, then each merged folder gets the note. fairyfly never
   deletes the old data; delete it yourself once you have checked the new folder.

A failure at any step leaves the old folders without the note, so the next start tries again; merging twice changes
nothing.

**Merging** never overwrites, with one exception, the pairing:

- A file the new folder lacks is copied with its modification time (to a temporary name first, then published
  without replacing anything).
- An audit trail file (`*.jsonl`) gets the complete records it lacks appended, one append-only write per record. A
  fairyfly writing to the same file at that moment loses nothing, no record is duplicated (also when the same folder
  is seen twice: a Store process sees `%LOCALAPPDATA%\fairyfly` merged with its private copy), and a last line without
  a line end in an old folder (a record still being written) is not copied. When the file in the new folder ends in a
  cut-off line, the records go to `<month>.migrated.jsonl` next to it instead: fairyfly never writes after a line it
  did not write.
- `link\state.json` (the pairing): when the old folders hold two different ones, the most recently modified one is
  used. The other stays where it was.
- Any other file that differs keeps the copy found first (`%LOCALAPPDATA%\fairyfly` before the package copy, the new
  folder before both).

Every conflict is printed as a warning (stderr, or the tray's log), recorded in `.data-dir.json`, and shown by
`fairyfly doctor` (check `data_dir`, warn, with the file names and both folders). `fairyfly link status` adds
`data_dir_conflicts` and a `warning` when it concerns the pairing.

**When the move fails** (the new folder cannot be written) or another fairyfly holds the lock for longer than 30 s,
the old folders stay untouched, fairyfly prints a warning and tries again at the next start. For that run the link
uses the pairing from `%LOCALAPPDATA%\fairyfly\link\state.json` when the new folder has none, so it does not look
unpaired; it logs that. With a required audit trail (`--audit-required`, `FAIRYFLY_AUDIT=required`,
`audit.required: true`) the command fails closed with `AUDIT_UNAVAILABLE`, naming the folder and the reason.

**An older fairyfly still running** (another installation that was not updated) keeps writing to
`%LOCALAPPDATA%\fairyfly`. Records it writes after the old folder got its note are not moved; update or remove it.
An MSI upgrade closes this installation's trays before it replaces the files (#127,
`fairyfly-tray.exe --close`), so this concerns a second installation only.

## Uninstall

The MSI's uninstall (`fairyfly-tray.exe --uninstall`) removes the trays' Run values and registry settings and the
notification registration, nothing else. Removing the Store package deletes only its private copies. In both cases
`%USERPROFILE%\.fairyfly` stays, like the audit trail it holds; delete it yourself if you no longer need it.
