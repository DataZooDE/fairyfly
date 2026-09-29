# fairyfly

fairyfly is a Windows command-line tool for automating SAP GUI through the SAP GUI Scripting COM API. It can attach to an open session, navigate to a transaction, interact with controls, and read the current screen as structured data. The project aims to expose these operations to AI clients through MCP, but the MCP server is not implemented yet.

## Current CLI

The registered commands are: attach, launch, login, disconnect, connections, list, tcode, click, fill, get, screen, press_f4, send-key, close, batch, doctor, and serve. The serve command currently returns NOT_IMPLEMENTED.

Typical use with an already open SAP session:

~~~powershell
.\build\bin\Release\fairyfly.exe doctor
.\build\bin\Release\fairyfly.exe attach
.\build\bin\Release\fairyfly.exe tcode SM59
.\build\bin\Release\fairyfly.exe screen read --output markdown
.\build\bin\Release\fairyfly.exe screen read
~~~

JSON output and `error` logging are the defaults, so routine commands need neither `--output json` nor `--log-level error`. Use `--log-level info` for operational messages or `--verbose` for debug logging.

When `list` shows multiple SAP GUI sessions, use `attach --session-id "/app/con[0]/ses[1]"` to save that exact session without mouse selection. Subsequent commands can target the returned numeric connection-file ID with `--connection`.

The CLI also supports TOON output, including formatted errors. Use --help on any command for current options. Screen reads can filter by element type, text, ID, or editability, and can skip tab or tree extraction when a screen is slow or problematic. Grid and table reads return up to 20 rows by default; use `screen read --max-rows 64` to request more (up to 200), with a longer read time on large screens. For classic `GuiUserArea` lists such as SE16, that limit applies to rows exposed by the current SAP GUI viewport; scroll to read later backend hits. TextEdit shells appear in JSON and Markdown screen reads. On a `GuiShell` with subtype `AbapEditor`, `get <element-id>` returns up to 200 redacted source lines as `value`, together with total/read line counts and a truncation flag. `screen read` and `screen find` probe only the FindById paths that Children is known to miss; if a screen has controls reachable only by ID inside a container that lists other children, pass `--probe-all` (or set `FAIRYFLY_PROBE_ALL=1`) to restore exhaustive probing of every container, at the cost of extra COM calls. When a requested tab cannot be read, `screen read --tab` returns `TAB_LOAD_FAILED` (with `tab_id` and `reason` `not_found` or `busy_timeout`) after restoring the original tab; a full tab expansion reports the failing tabs in an additive `tabs_failed` list and fails only if every tab failed.

For a control search without a full screen extraction, use `screen find --id-contains RSRD1-TBMA_VAL --connection 0`. The command can combine an ID substring, an ASCII case-insensitive `--name-contains`, and exact `--type`, and returns up to `--limit` matches (default 1, maximum 100). It searches the active window's controls, stops after the match limit, and reads text only from matching simple controls. Grid and tree matches return control identity, not their rows or nodes; use `screen read` for that content. Search does not expand other tabs or search text values, and stops after 500 distinct controls with `scan_limit_reached=true`. The result reports `scanned_count` and whether the match limit was reached. When the complete ID is known, direct `get <element-id>` is faster. Checkbox and radio-button matches (and `get`) also report `selected`.

`click <grid> --row N --column COL --doubleclick` double-clicks a GridView cell (SetCurrentCell then DoubleClickCurrentCell); without `--row`/`--column` it returns GRID_ROW_OPTIONS_REQUIRED. `send-key <key> [--window @active|wnd[N]]` sends a virtual key (`enter`, `f1`..`f12`, `shift+f1`..`shift+f12`, or a raw VKey number 0-99); an unknown key returns INVALID_VKEY. `close [--vkey 12]` sends F12 to the active popup and verifies the popup closed (NO_POPUP when only the main window is open, POPUP_STILL_OPEN when it stays). `screen menu` lists the menu bar as a tree of `{id, text, enabled, children}` without selecting anything; `screen menu --select "Runtime Errors/Display"` explicitly selects an item by text path (case-insensitive, `&` accelerators ignored).

Use `fill <element-id> --clear` to empty a text field or TextEdit shell. This works in Windows PowerShell 5.1, which can omit an empty quoted positional argument when launching a native executable.

`disconnect --connection <id>` removes Fairyfly's saved connection while leaving SAP GUI open. Add `--close-session` to close that SAP GUI session before removing its saved connection.

`launch <connection>` uses native SAP GUI COM by default and never reads credentials. For a fresh logon screen, run `login --connection <id>`; see "Credentials and login" below. If launch opens a connection but no session appears, it returns `SESSION_NOT_READY`. `launch <connection> --allow-sapshcut` explicitly enables a separate fallback that opens the SAP Logon entry without credentials when native COM cannot (the child command line is only `-sysname=<name> -maxgui`); use `login` or `launch --login` for authentication afterward. If SAP GUI Security asks for a shortcut decision, launch returns `SAP_GUI_SECURITY_PROMPT`; no session is attached until SAP GUI permits the connection. The local `trial.env` file (legacy plaintext credentials) stays ignored by Git; keep it private and migrate it as described below.

`batch [--file PATH] [--stop-on-error]` reads one command per line from stdin (or a file) and runs all of them in one process with one shared SAP handler, which avoids re-initialising COM for every call. A line is a JSON array of argv strings (`["tcode","SM37"]`) or shell-style words (double quotes; backslash escapes a space, quote or backslash). Blank lines and lines starting with `#` are ignored. Each line prints one compact JSON result (always JSON); a failing line does not stop the batch unless `--stop-on-error` is given, and the exit code is 1 if any line failed. Nested `batch` returns BATCH_NESTED, malformed lines return BATCH_PARSE_ERROR.

The global `--read-only` flag (or `FAIRYFLY_READ_ONLY=1`) refuses state-changing actions with READ_ONLY_REFUSED (the error includes element id, text, tooltip and matched rule): buttons, menus and toolbar ids for Save, Delete, Release, Stop, Post, Activate, Lock/Unlock, Create, Change, Cancel job and Execute in background, ids containing `&DELETE`/`&SAVE`/`&RELEASE` or `tbar[0]/btn[11]`, `send-key`/`close` outside an allowlist (only F1, F3, F4, F7, F8, F12, Shift+F3, raw page keys 80-83, and Enter on the main window only; Enter with a popup active is refused), grid/tree double-clicks whose element or cell/node text matches those words (a double-click can still trigger an application action the guard cannot see), menu paths with `&` accelerators normalized and the resolved menu item text re-checked, synthetic toolbar buttons judged by their tooltip/text, `screen menu --select` paths, tree context-menu items, and `fill` entirely unless `--allow-fill` is given. Navigation (tcode, Back, Refresh, Display, Details, Job log, grid row select and double-click) stays allowed. `fairyfly --read-only batch` applies the guard to every line.

`click --wait-for-window` compares only the active window id, title, transaction and status bar text. A click that changes only field or grid contents (for example a "Next page" control) is reported as `screen_changed:false` after the full `--timeout`; use `get` or `screen read` to confirm such changes.

## Credentials and login

Credentials live in the Windows Credential Manager under `fairyfly:<connection name>` (user, client, language and password; the password is never listed or printed).

~~~powershell
fairyfly credentials set Bigfox --user DEVELOPER --client 001      # prompts for the password (or add --password-stdin)
fairyfly credentials list
fairyfly credentials delete Bigfox
fairyfly credentials import-env trial.env --connection Bigfox --delete-file   # migrate a legacy plaintext file
~~~

After importing, rotate the SAP password: the old one sat in plaintext on disk. `credentials set` and `import-env` need a console or piped stdin and return CREDENTIALS_PROMPT_UNAVAILABLE inside `batch`; they are allowed under `--read-only` (they do not touch SAP) but are audited.

`login [--connection <id>]` takes credentials from exactly one source: `--credentials-stdin` (colon-separated `Username`, `Password`, `System ID`, optional `Language` and `New Password` lines), `--credentials-file PATH` (deprecated, prints a warning), `--credential NAME` (a stored entry), or, with no flag, the stored entry named like the saved connection. There is no implicit `./trial.env` fallback. The result reports `credential_source` and any warnings, never the password.

`launch <connection> --login [--credential NAME]` launches, waits for the session, then logs in through the scripting API (also after `--allow-sapshcut`; a password never goes on a sapshcut command line). The result keeps the launch fields and adds a `login` object (`transaction`, `credential_source`, `warnings`). If the launch worked but the login failed, the login error code is returned with `connection_open: true` and the launch data under `error.launch`; the connection is left open. `--login` is allowed under `--read-only` because authentication is not a business-state change.

## Audit trail

Every invocation, and every line inside `batch`, appends one JSON record to `%LOCALAPPDATA%\fairyfly\audit\YYYY-MM.jsonl` (UTC month). Audit is on by default.

- Control: `--no-audit`, `--audit-required` (fail with AUDIT_UNAVAILABLE when the file cannot be written), `FAIRYFLY_AUDIT=0|off|required`, `FAIRYFLY_AUDIT_FILE=<path>`. `--audit-required` wins over any disable.
- Record fields: timestamp, pid, command, redacted argv, connection, SAP system/client/user/transaction, read_only, batch_line, status, error_code, exit code, duration_ms.
- Never logged: error messages, screen content, cell values, passwords or other secrets (secret-looking option values and `fill` values are replaced by a placeholder), the Windows user name and the host name. Search terms are kept.
- Append failures print a single warning and never break a command, unless audit is required.
- The file is append-only by convention, not tamper-proof: any process of the same Windows user can edit it.

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

The CLI and its test suite are under active development. MCP (`serve` returns NOT_IMPLEMENTED) and cross-platform SAP GUI support remain future work. See [open work](docs/OPEN_WORK.md) for pending build measurements and behavior checks. The source tree and --help output are the authority for available commands; historical investigation notes in this repository may describe earlier behavior.

SAP automation runs under the permissions of the connected SAP user. Enabling GUI scripting may require both client and server configuration. Review actions before using the CLI on a production system.
