# fairyfly

fairyfly is a Windows command-line tool for automating SAP GUI through the SAP GUI Scripting COM API. It can attach to an open session, navigate to a transaction, interact with controls, and read the current screen as structured data. `fairyfly mcp` exposes these operations to AI clients as an MCP server over stdio (see [MCP server](#mcp-server)).

## Current CLI

Since 0.2.0 the CLI is a noun/verb tree (see [docs/MIGRATION_CLI.md](docs/MIGRATION_CLI.md) for the old flat command names, which no longer exist):

| Group | Commands |
|---|---|
| `session` | `list`, `attach`, `launch`, `login`, `disconnect` |
| `connection` | `list` |
| `screen` | `read`, `find`, `capture` |
| `menu` | `list`, `select` |
| `element` | `get`, `click`, `fill`, `f4` |
| `key` | `send` |
| `popup` | `close` |
| `transaction` | `start` |
| `credentials` | `list`, `set`, `delete`, `import-env` |
| `mcp` | starts the MCP server (stdio, or `--http`, `--tray`); `mcp tools [--markdown]`, `mcp token`, `mcp setup`, `mcp teardown`, `mcp cert export`, `mcp config`, `mcp client-config`, `mcp doctor` |
| root verbs | `doctor`, `batch` |

Global options (`--log-level`, `-v`, `--output`, `--verbose-errors`, `--read-only`, `--no-audit`, `--audit-required`, `--version`) work before and after the noun/verb: `fairyfly screen read --read-only` is the same as `fairyfly --read-only screen read`. `fairyfly --help` groups the commands by section; `fairyfly <group> --help` lists the verbs.

Typical use with an already open SAP session:

~~~powershell
.\build\bin\Release\fairyfly.exe doctor
.\build\bin\Release\fairyfly.exe session attach
.\build\bin\Release\fairyfly.exe transaction start SM59
.\build\bin\Release\fairyfly.exe screen read --output markdown
.\build\bin\Release\fairyfly.exe screen read
~~~

JSON output and `error` logging are the defaults, so routine commands need neither `--output json` nor `--log-level error`. Use `--log-level info` for operational messages or `--verbose` for debug logging.

When `session list` shows multiple SAP GUI sessions, use `session attach --session-id "/app/con[0]/ses[1]"` to save that exact session without mouse selection. Subsequent commands can target the returned numeric connection-file ID with `--connection`.

The CLI also supports TOON output, including formatted errors. Use --help on any command for current options. Screen reads can filter by element type, text, ID, or editability, and can skip tab or tree extraction when a screen is slow or problematic. Grid and table reads return up to 20 rows by default (`--offset N` starts at row N; the output reports `next_offset`, drops trailing empty padding rows and counts them as `empty_rows_trimmed`; `Total Rows` is the control's row count, `Visible Rows (viewport)` the rows SAP shows in its own viewport, `Returned Rows` the rows listed; `--text-contains` keeps only the matching rows; `--only-fields` and the other `--only-*` selectors report the grids they dropped as `suppressed`, `--only-tables` returns just the tables; redacted values read `[REDACTED: <reason>]`); use `screen read --max-rows 64` to request more (up to 200), with a longer read time on large screens. For classic `GuiUserArea` lists such as SE16, that limit applies to rows exposed by the current SAP GUI viewport; scroll to read later backend hits. TextEdit shells appear in JSON and Markdown screen reads. On a `GuiShell` with subtype `AbapEditor`, `element get <element-id>` returns up to 200 redacted source lines as `value`, together with total/read line counts and a truncation flag. `screen read` and `screen find` probe only the FindById paths that Children is known to miss; if a screen has controls reachable only by ID inside a container that lists other children, pass `--probe-all` (or set `FAIRYFLY_PROBE_ALL=1`) to restore exhaustive probing of every container, at the cost of extra COM calls. When a requested tab cannot be read, `screen read --tab` returns `TAB_LOAD_FAILED` (with `tab_id` and `reason` `not_found` or `busy_timeout`) after restoring the original tab; a full tab expansion reports the failing tabs in an additive `tabs_failed` list and fails only if every tab failed.

For a control search without a full screen extraction, use `screen find --id-contains RSRD1-TBMA_VAL --connection 0`. The command can combine an ID substring, an ASCII case-insensitive `--name-contains`, and exact `--type`, and returns up to `--limit` matches (default 1, maximum 100). It searches the active window's controls, stops after the match limit, and reads text only from matching simple controls. Grid and tree matches return control identity, not their rows or nodes; use `screen read` for that content. Search does not expand other tabs or search text values, and stops after 500 distinct controls with `scan_limit_reached=true`. The result reports `scanned_count` and whether the match limit was reached. When the complete ID is known, direct `element get <element-id>` is faster. Checkbox and radio-button matches (and `element get`) also report `selected`.

`element click <grid> --row N --column COL --doubleclick` double-clicks a GridView cell (SetCurrentCell then DoubleClickCurrentCell); without `--row`/`--column` it returns GRID_ROW_OPTIONS_REQUIRED. `key send <key> [--window @active|wnd[N]]` sends a virtual key (`enter`, `f1`..`f12`, `shift+f1`..`shift+f12`, or a raw VKey number 0-99); an unknown key returns INVALID_VKEY. `popup close [--vkey 12]` sends F12 to the active popup and verifies the popup closed (NO_POPUP when only the main window is open, POPUP_STILL_OPEN when it stays). `menu list` lists the menu bar as a tree of `{id, text, enabled, children}` without selecting anything; `menu select "Runtime Errors/Display"` explicitly selects an item by text path (case-insensitive, `&` accelerators ignored).

Use `element fill <element-id> --clear` to empty a text field or TextEdit shell. This works in Windows PowerShell 5.1, which can omit an empty quoted positional argument when launching a native executable.

`session disconnect --connection <id>` removes Fairyfly's saved connection while leaving SAP GUI open. Add `--close-session` to close that SAP GUI session before removing its saved connection.

`session launch <connection>` uses native SAP GUI COM by default and never reads credentials. For a fresh logon screen, run `session login --connection <id>`; see "Credentials and login" below. If launch opens a connection but no session appears, it returns `SESSION_NOT_READY`. `session launch <connection> --allow-sapshcut` explicitly enables a separate fallback that opens the SAP Logon entry without credentials when native COM cannot (the child command line is only `-sysname=<name> -maxgui`); use `session login` or `session launch --login` for authentication afterward. If SAP GUI Security asks for a shortcut decision, launch returns `SAP_GUI_SECURITY_PROMPT`; no session is attached until SAP GUI permits the connection. The local `trial.env` file (legacy plaintext credentials) stays ignored by Git; keep it private and migrate it as described below.

`batch [--file PATH] [--stop-on-error]` reads one command per line from stdin (or a file) and runs all of them in one process with one shared SAP handler, which avoids re-initialising COM for every call. A line is the CLI arguments without the program name: a JSON array of argv strings (`["transaction","start","SM37"]`) or shell-style words (double quotes; backslash escapes a space, quote or backslash). Blank lines and lines starting with `#` are ignored. Each line prints one compact JSON result (always JSON); a failing line does not stop the batch unless `--stop-on-error` is given, and the exit code is 1 if any line failed. Nested `batch` returns BATCH_NESTED, malformed lines return BATCH_PARSE_ERROR.

The global `--read-only` flag (or `FAIRYFLY_READ_ONLY=1`) refuses state-changing actions with READ_ONLY_REFUSED (the error includes element id, text, tooltip and matched rule): buttons, menus and toolbar ids for Save, Delete, Release, Stop, Post, Activate, Lock/Unlock, Create, Change, Cancel job and Execute in background, ids containing `&DELETE`/`&SAVE`/`&RELEASE` or `tbar[0]/btn[11]`, `key send`/`popup close` outside an allowlist (only F1, F3, F4, F7, F8, F12, Shift+F3, raw page keys 80-83, and Enter on the main window only; Enter with a popup active is refused), grid/tree double-clicks whose element or cell/node text matches those words (a double-click can still trigger an application action the guard cannot see), menu paths with `&` accelerators normalized and the resolved menu item text re-checked, synthetic toolbar buttons judged by their tooltip/text, `menu select` paths, tree context-menu items, and `element fill` entirely unless `--allow-fill` is given. Navigation (`transaction start`, Back, Refresh, Display, Details, Job log, grid row select and double-click) stays allowed. `fairyfly --read-only batch` applies the guard to every line.

`element click --wait-for-window` compares only the active window id, title, transaction and status bar text. A click that changes only field or grid contents (for example a "Next page" control) is reported as `screen_changed:false` after the full `--timeout`; use `element get` or `screen read` to confirm such changes.

## Credentials and login

Credentials live in the Windows Credential Manager under `fairyfly:<connection name>` (user, client, language and password; the password is never listed or printed).

~~~powershell
fairyfly credentials set Bigfox --user DEVELOPER --client 001      # prompts for the password (or add --password-stdin)
fairyfly credentials list
fairyfly credentials delete Bigfox
fairyfly credentials import-env trial.env --connection Bigfox --delete-file   # migrate a legacy plaintext file
~~~

After importing, rotate the SAP password: the old one sat in plaintext on disk. `credentials set` and `import-env` need a console or piped stdin and return CREDENTIALS_PROMPT_UNAVAILABLE inside `batch`; they are allowed under `--read-only` (they do not touch SAP) but are audited.

`session login [--connection <id>]` takes credentials from exactly one source: `--credentials-stdin` (colon-separated `Username`, `Password`, `System ID`, optional `Language` and `New Password` lines), `--credentials-file PATH` (deprecated, prints a warning), `--credential NAME` (a stored entry), or, with no flag, the stored entry named like the saved connection. There is no implicit `./trial.env` fallback. The result reports `credential_source` and any warnings, never the password.

`session launch <connection> --login [--credential NAME]` launches, waits for the session, then logs in through the scripting API (also after `--allow-sapshcut`; a password never goes on a sapshcut command line). The result keeps the launch fields and adds a `login` object (`transaction`, `credential_source`, `warnings`). If the launch worked but the login failed, the login error code is returned with `connection_open: true` and the launch data under `error.launch`; the connection is left open. `--login` is allowed under `--read-only` because authentication is not a business-state change.

`session login --multiple-logon fail|keep|end|terminate` (also on `session launch --login`) controls what happens when the user is already logged on and SAP shows "License Information for Multiple Logons". `fail` (default) leaves the dialog open and returns `LOGON_NOT_COMPLETED` with `reason: multiple_logon_dialog`, a hint and the dialog texts. `keep` continues without ending other logons (success adds `multiple_logon`). `terminate` ends the new logon and returns `MULTIPLE_LOGON_TERMINATED` (SAP closes the session). `end` ends the user's other logons (unsaved data there is lost); it is never the default and is refused under `--read-only`.

## Audit trail

Every invocation, and every line inside `batch`, appends one JSON record to `%LOCALAPPDATA%\fairyfly\audit\YYYY-MM.jsonl` (UTC month). Audit is on by default.

- Control: `--no-audit`, `--audit-required` (fail with AUDIT_UNAVAILABLE when the file cannot be written), `FAIRYFLY_AUDIT=0|off|required`, `FAIRYFLY_AUDIT_FILE=<path>`. `--audit-required` wins over any disable.
- Record fields: timestamp, pid, command, redacted argv, connection, SAP system/client/user/transaction, read_only, batch_line, status, error_code, exit code, duration_ms.
- Never logged: error messages, screen content, cell values, passwords or other secrets (secret-looking option values and `element fill` values are replaced by a placeholder), the Windows user name and the host name. Search terms are kept.
- Append failures print a single warning and never break a command, unless audit is required.
- The file is append-only by convention, not tamper-proof: any process of the same Windows user can edit it.

## MCP server

`fairyfly mcp` is a Model Context Protocol server with 21 `gui_*` tools named `gui_<noun>_<verb>` after the CLI path (`gui_screen_read`, `gui_element_click`, `gui_transaction_start`, ..., `gui_batch`, and `gui_element_fill` in write mode). It starts in read-only guard mode; `mcp --allow-write` enables write mode, `mcp --tools screen,element` exposes only some tool families, and `FAIRYFLY_READ_ONLY=1` is a hard cap. Every tool call is audited (`audit_source: "mcp"`); no tool accepts a password. Two transports:

- **stdio** (default): a local MCP client launches it. Do not run it in an interactive console.
- **HTTP/HTTPS** (`mcp --http`): `POST /mcp` served by http.sys (127.0.0.1:8383 plain for development, HTTPS on 8443 after `mcp setup`) for remote clients (for example Claude Code on Linux), with named bearer tokens (`mcp token create|list|revoke|rotate`, stored hashed in the Windows Credential Manager) that carry scopes per tool family, a read-only flag, SAP system and T-code allowlists, a rate limit, an IP binding and an expiry. TLS is terminated in the kernel by http.sys: `mcp setup` (one elevated step) creates the certificate, URL reservation and TLS binding, `mcp teardown` removes them, `mcp cert export` gives clients the certificate; a server-level `--allow-ip` list and token IP binding judge the real peer address (no proxy trust). It serves both MCP HTTP protocol generations (legacy 2025-06-18/2025-11-25 and stateless 2026-07-28) with optional SSE. SAP GUI needs an interactive desktop, so it is a console app, or with `--tray` a system-tray app with logon autostart, not a Windows service. Also: YAML config (`mcp config init|show|validate|path`), paste-ready client snippets (`mcp client-config`) and `mcp doctor`.

~~~powershell
claude mcp add fairyfly -- C:\path\to\fairyfly.exe mcp                      # local, stdio
fairyfly mcp token create linux-reader --scope session,connection,screen   # remote: create a token (shown once)
fairyfly mcp --http                                                          # remote: serve on 127.0.0.1:8383
~~~

Setup for Claude Code, Claude Desktop and MCP Inspector, the full tool list (`fairyfly mcp tools`), options, safety model, audit trail and troubleshooting are in [docs/MCP.md](docs/MCP.md); example configs are under docs/examples/. The remote deployment (architecture, threat model, protocol and status codes, client cookbook, Linux check list) is in [docs/MCP_REMOTE.md](docs/MCP_REMOTE.md), with [docs/MCP_SETUP.md](docs/MCP_SETUP.md) for setup, doctor and teardown and [docs/MCP_TRAY.md](docs/MCP_TRAY.md) for the tray, YAML config, client-config and doctor. Live checks: `tests/integration/mcp_smoke.ps1` (stdio) and `tests/integration/mcp_http_smoke.ps1` (HTTP).

## Build

Requirements: Windows, SAP GUI for Windows with scripting enabled for live automation, Visual Studio 2022 C++ tools, CMake 3.20+, and vcpkg. Set VCPKG_ROOT to your vcpkg checkout. The project uses C++20 and the x64-windows-static triplet.

~~~powershell
cmake -S . -B build -G "Visual Studio 17 2022" -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release --target fairyfly --parallel
~~~

Run `build/bin/Release/fairyfly.exe` after a Release build. CMake stages this copy automatically. An older direct Visual Studio output file once failed to attach to SAP GUI on this workstation; refreshing it and relinking restored the direct path, while the staged copy remains the documented execution path.

For unit tests:

~~~powershell
cmake --build build --config Release --target unit_tests --parallel
ctest --test-dir build -C Release --output-on-failure
~~~

The Makefile provides build shortcuts. Routine builds target the CLI; test targets build the unit test executable. See [the build guide](docs/BUILD_OPTIMIZATION.md) for current performance notes.

## Project layout

- src/commands/ contains the CLI commands.
- src/com/ and src/com_automation_engine.cpp wrap SAP GUI COM operations.
- src/screen_reader.cpp, screen_element_collector.cpp, and the formatters extract and render screen data.
- tests/unit/ contains Catch2 tests; tests/integration/ contains scripts requiring a live SAP GUI session.
- CMakeLists.txt builds the shared fairyfly_core static library and the CLI.

## Status

The CLI and its test suite are under active development. Version 2026.09.30. The MCP server offers stdio and, for remote use, HTTPS through http.sys (stateless 2026-07-28 and legacy protocol eras); cross-platform SAP GUI support remains future work. See [open work](docs/OPEN_WORK.md) for pending build measurements and behavior checks. The source tree and --help output are the authority for available commands; historical investigation notes in this repository may describe earlier behavior.

SAP automation runs under the permissions of the connected SAP user. Enabling GUI scripting may require both client and server configuration. Review actions before using the CLI on a production system.
