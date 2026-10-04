# Command line guide

`fairyfly.exe` is a command line program. Every command prints one structured result (JSON by default) and exits
with 0 on success, so it is easy to call from PowerShell, Python, schedulers and AI agents. The MCP server
([MCP.md](MCP.md)) runs exactly these commands, so everything on this page also describes what the `gui_*` tools do.

`fairyfly --help` prints the complete reference of the version you have, including usage notes and an example for
every command. `fairyfly <group> --help` limits it to one group, and `fairyfly <group> <verb> --help` to one command.
Help works without SAP.

## Commands at a glance

| Group | Commands | Purpose |
|---|---|---|
| `session` | `list`, `attach`, `launch`, `login`, `disconnect` | Find, open, log on to and release SAP GUI sessions |
| `connection` | `list` | Show the sessions fairyfly has saved |
| `transaction` | `start` | Run a transaction code |
| `screen` | `read`, `find`, `capture` | Read the screen as data, search for controls, take a screenshot |
| `element` | `get`, `click`, `fill`, `f4` | Read, click or fill one control, open value help |
| `key` | `send` | Send Enter, F-keys or page keys |
| `popup` | `close` | Close the active popup |
| `menu` | `list`, `select` | Show or select menu bar items |
| `credentials` | `list`, `set`, `delete`, `import-env` | Manage SAP logon data in the Windows Credential Manager |
| `mcp` | server, `tools`, `token`, `setup`, `teardown`, `cert export`, `config`, `client-config`, `doctor` | The MCP server for AI agents ([MCP.md](MCP.md), [MCP_REMOTE.md](MCP_REMOTE.md)) |
| (root) | `doctor`, `batch` | Environment check; many commands in one process |

## Global options

These work before and after the command: `fairyfly screen read --read-only` is the same as
`fairyfly --read-only screen read`.

| Option | Effect |
|---|---|
| `--output json\|markdown\|toon` | Result format. JSON is the default; Markdown is easy to read; TOON is a compact text format for LLMs. Errors use the same format. |
| `--read-only` | Refuse state-changing actions (see [Read-only guard](#read-only-guard)). Also `FAIRYFLY_READ_ONLY=1`. |
| `--log-level LEVEL`, `-v` | Log verbosity on stderr (`error` is the default; `info`, `debug`; `-v` = debug). |
| `--verbose-errors` | Add detailed suggestions to error results. |
| `--no-audit`, `--audit-required` | Turn the audit trail off for this run, or fail when it cannot be written. |
| `--version` | Print the version. |

## A typical session

~~~powershell
fairyfly doctor                                  # is SAP GUI ready for scripting?
fairyfly session list                            # open sessions and their ids
fairyfly session attach --session-id "/app/con[0]/ses[0]"   # save it (connection_id in the result)
fairyfly transaction start SM37                  # open a transaction
fairyfly screen read --output markdown           # read the screen
fairyfly element fill "wnd[0]/usr/ctxtJOBNAME" "Z*" # type into a field (not allowed with --read-only)
fairyfly key send f8                             # execute
fairyfly screen read --only-tables                # read the result list
~~~

Element ids such as `wnd[0]/usr/ctxtJOBNAME` come from `screen read` or `screen find`.

## Sessions and connections

- `session list` shows all open SAP GUI connections and sessions with their ids.
  `session attach --session-id "/app/con[0]/ses[1]"` saves one of them as a fairyfly *connection*; without
  `--session-id` it asks you to click into the SAP GUI window. The result contains the `connection_id`; pass it as
  `--connection <id>` to later commands (with a single saved connection it can be omitted).
- `session launch <SAP Logon entry>` opens an entry of SAP Logon through SAP GUI's own COM interface. It never reads
  credentials. Add `--login` to log on right after launch (see [Credentials and logon](#credentials-and-logon)).
  If launch opens a connection but no session appears, it returns `SESSION_NOT_READY`. If SAP GUI Security asks the
  user to allow the connection, it returns `SAP_GUI_SECURITY_PROMPT`.
- `session launch <entry> --allow-sapshcut` enables a fallback that starts the entry through `sapshcut` when native
  COM cannot (only `-sysname=<name> -maxgui`, never a password).
- `session disconnect --connection <id>` removes fairyfly's saved connection and leaves SAP GUI open.
  `--close-session` also closes that SAP GUI session.
- `connection list` shows the saved connections; `--cleanup` removes stale ones.
- `session list` and `session attach` report `server_time: null` with `server_time_source: "unavailable"` (the
  scripting API has no SAP server clock) plus the PC clock as `client_time`.

## Reading the screen

`screen read` returns the active window as structured data: fields with labels and values, buttons, tabs, tables
and grids, trees, text editors and the status bar.

Make reads smaller and faster:

| Option | Effect |
|---|---|
| `--only-fields`, `--only-buttons`, `--only-editable`, `--only-f4-fields`, `--only-tables` | Only one kind of element. Grids that an `--only-*` option drops are reported as `suppressed`. |
| `--text-contains TEXT`, `--id-contains ID`, `--type TYPE` | Filter elements (and grid rows) by text, id or type. With tabs, the result lists `tabs_searched`, `tabs_skipped` and a `text_filter_note`. |
| `--tab ID`, `--no-tabs` | Read one tab only, or skip the other tabs. Reading all tabs selects each one in turn. |
| `--max-rows N`, `--offset N` | Grid and table rows: 20 by default, up to 200. The result reports `next_offset` for paging. |
| `--compact`, `--first` | Shorter Markdown (no ids, empty fields collapsed); only the first matching element. |
| `--skip-trees`, `--no-children` | Skip tree extraction or child elements, for slow or problematic screens. |

Grid results report `Total Rows` (the control's row count), `Visible Rows (viewport)` (what SAP shows),
`Returned Rows` and `empty_rows_trimmed` (trailing empty padding rows that were dropped). For classic
`GuiUserArea` lists such as SE16, the row limit applies to rows in the current viewport; scroll to read later hits.
Sensitive values read `[REDACTED: <reason>]`.

Tabs: an unknown `--tab` returns `TAB_NOT_FOUND` with `available_tabs`. A tab that exists but cannot be read returns
`TAB_LOAD_FAILED` after the original tab has been restored. When all tabs are expanded, failing tabs are listed in
`tabs_failed`, and the read fails only if every tab failed.

Other details:

- Text editors (`GuiTextedit`) appear in JSON and Markdown reads. For an ABAP editor shell, `element get <id>` returns
  up to 200 redacted source lines with line counts and a truncation flag.
- How the screen is gathered: `FAIRYFLY_SCREEN_READER=auto` (default) reads the whole screen tree with one
  `GetObjectTree` call (SAP GUI 7.70 PL3 or later; about 35% faster) and falls back to the per-element reader when
  that call is unavailable, recording the reason in `diagnostics.screen_reader`. `bulk` reports
  `OBJECT_TREE_UNAVAILABLE` instead of falling back; `legacy` always reads element by element. A fault in the bulk
  call switches it off for the rest of the process.
- `--probe-all` (or `FAIRYFLY_PROBE_ALL=1`) probes every container for controls that are reachable only by id. It is
  slower and rarely needed.

### Finding controls

`screen find --id-contains RSRD1-TBMA_VAL` searches the active window without a full read. Combine `--id-contains`,
`--name-contains` (case-insensitive) and `--type`; `--limit` sets the number of matches (default 1, maximum 100).
Grid and tree matches return the control, not its rows; use `screen read` for content. The search stops after 500
controls (`scan_limit_reached`). When the full id is known, `element get <id>` is faster. Check boxes and radio
buttons report `selected`; buttons, tabs, check boxes and radio buttons report their `tooltip`.

### Screenshots

`screen capture --file shot.png` saves a PNG (`--format base64` for text output). Crops (`--x --y --width --height`) are in native window pixels and are applied before
`--scale`. The result reports `native_size`, `crop` and `output_size`. A crop outside the window is
`INVALID_ARGUMENT`.

## Acting on the screen

- `element click <id>` clicks a button, tab, check box, radio button, tree node or grid cell. On grids,
  `--row N --column COL --doubleclick` double-clicks a cell. `--wait-for-window` waits for the window id, title,
  transaction or status bar to change. A click that changes only field or grid contents (for example "next page") is
  reported as `screen_changed: false` after `--timeout`; confirm it with `element get` or `screen read`.
- `element fill <id> <value>` types into a field, combo box, check box or grid cell. `--clear` empties a field (use it
  instead of an empty quoted argument, which Windows PowerShell 5.1 may drop). The result echoes the value as the
  control shows it (max. 200 characters, redacted for password fields) and a `field` object with `type`,
  `max_length`, `input_kind` (date, time, numeric), `format_hint` such as `DD.MM.YYYY` and, when SAP reformatted a
  date, a `format_warning`.
- `element get <id>` reads one element. An element on a tab that is not selected returns `ELEMENT_ON_INACTIVE_TAB`
  (with `tab_id` and `tab_text`); `--activate-tab` selects the tab for the read and restores the previous one.
- `element f4 <id>` opens the value help of a field.
- `key send <key> [--window @active|wnd[N]]` sends `enter`, `f1`..`f12`, `shift+f1`..`shift+f12`, the page keys or a
  raw VKey number 0-99. An unknown key returns `INVALID_VKEY`.
- `popup close [--vkey 12]` sends F12 to the active popup and checks that it closed (`NO_POPUP`, `POPUP_STILL_OPEN`).
- `menu list` returns the menu bar as a tree without selecting anything. `menu select "Runtime Errors/Display"`
  selects an item by its text path (case-insensitive, `&` accelerators ignored).
- Status messages: an action that leaves an information, success or warning message succeeds and reports
  `status_message`. Error and abort messages (type E and A) fail with `ACTION_FAILED` or `ACTION_ABORTED`.

## Batch mode

`batch [--file PATH] [--stop-on-error]` reads one command per line from stdin or a file and runs them in one process
with one SAP connection, which avoids starting fairyfly for each step. A line is the command without the program
name, either as a JSON array (`["transaction","start","SM37"]`) or as shell-style words (double quotes; a backslash
escapes a space, quote or backslash). Blank lines and lines starting with `#` are ignored. Each line prints one compact
JSON result. A failing line does not stop the batch unless `--stop-on-error` is given; the exit code is 1 if any line
failed. `fairyfly --read-only batch` applies the read-only guard to every line.

~~~powershell
@'
transaction start SM37
screen read --only-fields
'@ | fairyfly batch
~~~

## Read-only guard

`--read-only` (or `FAIRYFLY_READ_ONLY=1`) refuses state-changing actions with `READ_ONLY_REFUSED`. The error names the
element, its text and tooltip and the rule that matched. Refused are:

- buttons, menus and toolbar entries for Save, Delete, Release, Stop, Post, Activate, Lock/Unlock, Create, Change,
  Cancel job and Execute in background, ids containing `&DELETE`, `&SAVE`, `&RELEASE` or `tbar[0]/btn[11]`, and
  synthetic toolbar buttons judged by their tooltip or text;
- `key send` and `popup close` outside an allowlist (F1, F3, F4, F7, F8, F12, Shift+F3, the page keys, and Enter on
  the main window only; Enter while a popup is open is refused);
- grid and tree double-clicks whose element or cell text matches those words, tree context-menu items, and
  `menu select` paths (the resolved menu text is checked again);
- `element fill`, unless `--allow-fill` is given.

Navigation stays allowed: `transaction start`, Back, Refresh, Display, Details, job logs, selecting grid rows and
double-clicking, and the allowlisted keys (F8 executes a report). Clicks on check boxes and radio buttons stay
allowed too and change their selection. The text rules use English words, so with another logon language only the
id-based rules apply. A double-click can still start an application action that the guard cannot see, so read-only
mode is a strong guard rail, not a guarantee. SAP authorizations remain the real limit. See
[SECURITY.md](SECURITY.md#read-only-guard-mode-and-write-mode).

## Credentials and logon

SAP logon data lives in the Windows Credential Manager under `fairyfly:<name>` (user, client, language and password).
The password is never listed or printed.

~~~powershell
fairyfly credentials set Bigfox --user DEVELOPER --client 001   # prompts for the password (or --password-stdin)
fairyfly credentials list
fairyfly credentials delete Bigfox
fairyfly credentials import-env old.env --connection Bigfox --delete-file   # migrate a legacy plaintext file
~~~

After importing a plaintext file, change the SAP password: the old one was on disk. `credentials set` and
`import-env` need a console or piped stdin (`CREDENTIALS_PROMPT_UNAVAILABLE` inside `batch`). They are allowed under
`--read-only` because they do not touch SAP, and they are audited.

`session login [--connection <id>]` logs on to an open logon screen with credentials from exactly one source:

- `--credential NAME`: a stored entry;
- no flag: the stored entry named like the saved connection;
- `--credentials-stdin`: `Username`, `Password`, `System ID` and optional `Language` and `New Password` lines,
  colon-separated;
- `--credentials-file PATH` (deprecated, prints a warning).

The result reports `credential_source` and warnings, never the password.

`session launch <entry> --login [--credential NAME]` launches, waits for the session and logs on through the
scripting API. If the launch worked but the logon failed, the logon error is returned with `connection_open: true`
and the connection stays open. Logon is allowed under `--read-only` because authentication changes no business data.

`--multiple-logon fail|keep|end|terminate` (on `session login` and `session launch --login`) decides what happens when
SAP shows "License Information for Multiple Logons":

| Value | Effect |
|---|---|
| `fail` (default) | Leave the dialog open and return `LOGON_NOT_COMPLETED` with `reason: multiple_logon_dialog` |
| `keep` | Continue without ending the other logons |
| `terminate` | End the new logon (`MULTIPLE_LOGON_TERMINATED`) |
| `end` | End the user's other logons (unsaved data there is lost); refused under `--read-only` |

## Audit trail

Every command, and every line of a `batch`, adds one JSON record to `%LOCALAPPDATA%\fairyfly\audit\YYYY-MM.jsonl`
(UTC month). It is on by default.

- Recorded: timestamp, process id, command, redacted arguments, connection, SAP system, client, user and transaction,
  read-only flag, batch line, status, error code, exit code and duration.
- Never recorded: error messages, screen content, cell values, passwords and other secrets (secret-looking option
  values and `element fill` values are replaced by a placeholder), the Windows user name and the host name. Search
  terms are kept.
- Control: `--no-audit`, `--audit-required` (fail with `AUDIT_UNAVAILABLE` when the file cannot be written; wins over
  any disable), `FAIRYFLY_AUDIT=0|off|required`, `FAIRYFLY_AUDIT_FILE=<path>`. The record is written after the
  command ran: `AUDIT_UNAVAILABLE` does not mean the action did not happen, so do not retry it blindly.
- A write failure prints one warning and never breaks a command, unless audit is required.
- The file is append-only by convention, not tamper-proof: any process of the same Windows user can edit it.

~~~powershell
Get-Content $env:LOCALAPPDATA\fairyfly\audit\2026-10.jsonl | ConvertFrom-Json |
  Select-Object ts, cmd, status, error_code, duration_ms
~~~

## Environment variables

| Variable | Effect |
|---|---|
| `FAIRYFLY_READ_ONLY=1` | Read-only guard for every command; hard cap for the MCP server |
| `FAIRYFLY_AUDIT`, `FAIRYFLY_AUDIT_FILE` | Audit trail on/off/required and its path |
| `FAIRYFLY_SCREEN_READER=auto\|bulk\|legacy` | How `screen read` gathers the screen |
| `FAIRYFLY_PROBE_ALL=1` | Exhaustive control probing (as `--probe-all`) |
| `FAIRYFLY_MCP_*` | MCP server configuration ([MCP_TRAY.md](MCP_TRAY.md#yaml-config)) |

## Migrating from the flat commands

The noun/verb tree (first released as 0.2.0, before the switch to calendar versions) replaced the flat command
names without aliases. Old names fail with the normal "not expected" error, and old `sap_*` MCP tool names are
unknown tools. Every command kept its options and behavior; only its path changed. The mapping is defined in
`src/command_table.cpp`.

| Old command | New command |
|---|---|
| `list` | `session list` |
| `attach` | `session attach` |
| `launch NAME` | `session launch NAME` |
| `login` | `session login` |
| `disconnect` | `session disconnect` |
| `connections` | `connection list` |
| `screen read` / `screen find` / `screen capture` | unchanged |
| `screen menu` | `menu list` |
| `screen menu --select PATH` | `menu select PATH` (positional) |
| `get` | `element get` |
| `click` | `element click` |
| `fill` | `element fill` |
| `press_f4` | `element f4` |
| `send-key KEY` | `key send KEY` |
| `close` | `popup close` |
| `tcode CODE` | `transaction start CODE` |
| `credentials list\|set\|delete\|import-env` | unchanged |
| `doctor`, `batch` | unchanged |
| `serve [options]` | `mcp [options]` |

What changed with it:

- `batch` lines use the new paths (`["element","click","wnd[0]/usr/btn[1]"]`). `mcp` inside `batch` returns
  `MCP_UNAVAILABLE` (formerly `SERVE_UNAVAILABLE`).
- The audit `cmd` field holds the space-joined path (`element click`, `transaction start`; formerly `click`, `tcode`).
  Server lifecycle records use `cmd: "mcp"` (formerly `serve`). Queries over old and new files need to match both.
- Hints in error results name the new commands. JSON result fields did not change.
- MCP tools are named `gui_` plus the command path; tool arguments did not change. Claude Code shows them as
  `mcp__fairyfly__gui_*`, so permission rules that mention `mcp__fairyfly__sap_*` must be updated, and `gui_batch`
  items must use the new names. MCP client configs must launch `fairyfly.exe mcp` instead of `serve`.

| Old tool | New tool | Old tool | New tool |
|---|---|---|---|
| `sap_sessions` | `gui_session_list` | `sap_get` | `gui_element_get` |
| `sap_attach` | `gui_session_attach` | `sap_click` | `gui_element_click` |
| `sap_launch` | `gui_session_launch` | `sap_fill` | `gui_element_fill` |
| `sap_login` | `gui_session_login` | `sap_press_f4` | `gui_element_f4` |
| `sap_disconnect` | `gui_session_disconnect` | `sap_send_key` | `gui_key_send` |
| `sap_connections` | `gui_connection_list` | `sap_close_popup` | `gui_popup_close` |
| `sap_screen_read` | `gui_screen_read` | `sap_tcode` | `gui_transaction_start` |
| `sap_screen_find` | `gui_screen_find` | `sap_credentials_list` | `gui_credentials_list` |
| `sap_capture` | `gui_screen_capture` | `sap_doctor` | `gui_doctor` |
| `sap_menu_list` | `gui_menu_list` | `sap_batch` | `gui_batch` |
| `sap_menu_select` | `gui_menu_select` | | |
