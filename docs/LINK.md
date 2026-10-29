# Cloud link (`fairyfly link`)

`fairyfly link` connects this desktop's fairyfly to the fairyfly cloud gateway, so that AI assistants your
organisation has connected there can use your SAP GUI:
- Supported assistants: Gemini Enterprise, Claude, ChatGPT and Copilot Studio.
- The connection is outbound only: no port is opened on the PC and no VPN is needed.
- The cloud side is described in the gateway's documentation (research repository,
  `2026-10-14-fairyfly-cloud-agent-access/gateway`).

| Command | What it does |
|---|---|
| `fairyfly link pair [--gateway https://fairyfly.io]` | Shows a QR code and a number. Scan the code with your phone, sign in, type the number. The desktop then shows **which account and organisation** it would act for (and whether the identity provider verified the e-mail address); type `yes` only if that is you |
| `fairyfly link status` | Paired or not, with device, account, organisation and gateway |
| `fairyfly link run [--read-only] [--tools session,screen] [--approve desktop\|no]` | Connects and serves tool calls until Ctrl+C |
| `fairyfly link run --tray [--install-autostart\|--remove-autostart]` | The same link in the system tray, without the MCP server (see [Tray](#tray)) |
| `fairyfly link pause` | Disconnects a running link until resumed: the AI assistant cannot reach this desktop meanwhile (the gateway answers `DEVICE_OFFLINE`). Works from any console; the running link follows within a second |
| `fairyfly link resume` | Lets a paused link connect again |
| `fairyfly link unpair` | Forgets the pairing here (remove the desktop in the web app too) |

Without `--gateway`, `link pair` uses `FAIRYFLY_LINK_GATEWAY`. With neither it asks for the service's web address
with `https://fairyfly.io` already filled in (in a console it stands in the input line and can be edited; from a pipe
it is shown as `[https://fairyfly.io]`): Enter accepts it.
The address must be `https://`; plain `http://` is accepted only for exactly `localhost`, `127.0.0.1` or `[::1]`
(optional port, no `user@`) for a gateway on this computer. Anything else is refused with `INVALID_ARGUMENT` before
it is contacted. The link address the gateway hands out at pairing must be `wss://` (`ws://` only to those three
hosts), otherwise nothing is saved; a stored pairing with another address is refused by `link run` and `link status`
with `LINK_STATE_INSECURE` (pair again).

One link per pairing: `link run`, in a console or in the link tray, holds a lock for its pairing state file while it
serves. A second one for the same file is refused with `LINK_ALREADY_RUNNING` and exit code 3 (quit the first: tray
Quit, console Ctrl+C). The link tray does not fail in that case, also when the console's link started just before
the tray's: it says so in a notice and connects once the console's link ends. `link pair` reports the next step accordingly: nothing when the link tray is running (it connects by
itself), and a restart when a link for this pairing is already running with the previous pairing.

What the console shows:
- `link pair` prints `Contacting <gateway>…` at once, then the QR code, the typed code and the number.
- The QR code uses block characters where the console can show them. Elsewhere (stderr redirected to a file or pipe,
  or a raster font whose code page has no block characters) it falls back to ASCII (`##`).
  - `--qr-style blocks|ascii` overrides that choice.
  - `--qr-png FILE` also writes the code as a picture.
- Text reaches a Windows console through `WriteConsoleW`, so its code page (437, 1252, ...) does not matter.
- `link run` prints one line for each change:
  - `connected to <host> as <account> in '<organisation>' – Ctrl+C stops`;
  - when the connection is lost, the reason and when it reconnects, then `reconnected to <host>`;
  - pause and resume.
- Ctrl+C stops both commands within a second; a second Ctrl+C ends the process at once. A cancelled pairing saves
  nothing (`PAIRING_CANCELLED`).
- `pair`, `status`, `pause`, `resume` and `unpair` never touch SAP GUI: unlike the SAP commands, they run without
  attaching to SAP GUI first.
- `--log-level debug` also shows how long the gateway took to answer, and libffl's own log messages.

Files, all in `%USERPROFILE%\.fairyfly\link\`:
- `state.json`: the pairing. The device key inside it is protected with DPAPI.
- `state.json.paused`: exists while the link is paused.
- `state.json.status.json`: written by the running link (`connected`, `paused`, `stopped`, with its process id,
  executable, start time and the time of writing). `link status` and the tray read it: when that process has ended,
  or its id now belongs to another program, the link is `not running` at once (`link status` then adds `last_seen`:
  the state, time and process id the link last wrote); a live link whose status is older than 60 s (hung) is
  `not running` as well.

## Tray

`fairyfly link run --tray` runs the link in the system tray. It has its own icon and does not start the MCP server
or an HTTP endpoint.
- The Start-menu entry **fairyfly** (installed by `fairyfly.msi` / winget) runs exactly this command through
  `fairyfly-tray.exe`, a small Windows program next to `fairyfly.exe` that starts it without a console window. The
  installer starts the tray after an interactive install (not with `/qn`; `LAUNCHAPP=0` turns it off).
- It detaches from the console, like `mcp --tray`. Only one link tray runs per Windows session: starting it again
  (Start menu) opens the running tray's menu at its icon.
- The icon is teal (the local MCP server's tray is violet). Left or right click opens the menu; its first line, in
  bold, is the status.
- A desktop that is not paired still gets the tray (grey icon): a notice says so, and the menu offers
  **Pair with the fairyfly cloud...** (`link pair` in a console, which asks for the service's web address with
  `https://fairyfly.io` filled in, unless `FAIRYFLY_LINK_GATEWAY` is set). The link starts by itself as soon as the pairing is saved.
- An error while starting (for example a failed relaunch) is shown in a message box, not only in the
  console, which closes at once when the tray was started from the Start menu or the autostart.
- The icon is teal while connected, has an amber badge while paused and is faded otherwise. It also has the amber
  badge while connected when the desktop clock is off or a call has waited 60 s for SAP GUI (see "What runs where").
- A notice appears when the link starts, pauses, resumes or stops.
- The menu has:
  - the pairing (organisation, account) and the link's state;
  - while the audit trail is on: *Audit trail on — your admin records steps and screenshots on this desktop* (see
    "Audit trail" below; a balloon also tells when it turns on or off);
  - **Pause** / **Resume**;
  - **Allow changes in SAP** (a check mark): the standing approval at this desktop. On, the cloud's changes run
    without asking; off (the default), every change asks at this desktop (see "Approval" below). Turning it on asks
    first. It is saved for the Windows user (`HKCU\Software\DataZoo\fairyfly\tray`, value `link_allow_changes`), so it
    stays as it is after a restart and an update; uninstalling removes it. It is greyed out when your organisation
    requires approval ("Approval is required by your administrator") and with `--read-only` / `FAIRYFLY_READ_ONLY=1`.
    Changes always need the organisation's grant;
  - **Health check**: `link status` in a console;
  - **Pair again**: `link pair` in a console (pairing needs the person at the desktop);
  - **Start local MCP server**: starts the MCP server in its own tray (`fairyfly mcp --tray --http`, plain HTTP on
    `127.0.0.1:8383` unless the config file says otherwise; see [MCP_TRAY.md](MCP_TRAY.md)). While it runs the item is
    checked and disabled (it follows the MCP tray's single-instance guard `Local\fairyfly-tray`); quit it from its own
    icon;
  - **Start with Windows** (a check mark): the Run value `fairyfly-link` (below). It is turned on once, at the first
    start from the Start menu or the installer (`fairyfly-tray.exe`); after that only this item changes it. A Run value
    of an older version that starts `fairyfly.exe` directly is moved to `fairyfly-tray.exe`, its options kept;
  - **Open log** (`%USERPROFILE%\.fairyfly\logs\link.log`), **Open audit trail folder**, **Check for updates**;
  - **Quit**, which disconnects the link and ends the process.
- The other `link run` options apply as usual: `fairyfly link run --tray --tools session,screen`. `--allow-write` is
  still accepted and has no effect (changes are possible, each approved).
- If the link ends with an error, a message box points to the log.

Autostart at logon:

    fairyfly link run --tray --install-autostart [--tools ...]
    fairyfly link run --tray --remove-autostart

This writes or removes the value `fairyfly-link` under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`. The
value holds the `link run --tray` command with the options given, without the autostart flag, and starts
`fairyfly-tray.exe` when it is installed (no console at logon). It is independent of the MCP tray's `fairyfly-mcp`
value.

`fairyfly mcp --tray` also has a **Cloud link** submenu:
- the pairing (organisation, account) and the link's state;
- **Connect to cloud...** or **Pair again**: `link pair` in a console (pairing needs the person at the desktop);
- **Start link**: starts the link tray (`fairyfly link run --tray`);
- **Pause** / **Resume**;
- **Health check**: `link status` in a console.

## Audit trail

Your organisation's admins can turn on, per desktop, an audit trail of what AI assistants do on it: each step with a
short description, the SAP fields it changed (old → new) and screenshots of the SAP window before and after it. It is
**off** until an admin turns it on in the web app (Admin › Desktops). The contract with the gateway is
`docs/AUDIT_TRAIL.md` in fairyfly-gateway; the protocol work (announcing `trail/1` and `trail-blur/1`, the policy,
the `step` and `shot` frames, the limits) is libffl's. fairyfly implements the recorder
(`src/link/trail_recorder.cpp`, SAP GUI access in `src/link/trail_screen_source.cpp`).

**When it records.** Only while the gateway's policy for this desktop is on **and** the call asks for a record. A
gateway without the audit trail, or an admin who turns it off, means nothing is recorded and no notice is shown.

**The notice.** While the trail is on, the person at this desktop sees it:
- `link run` prints a line when it turns on or off, e.g. `Audit trail ON: your administrator records the steps
  assistants take on this desktop, with screenshots (kept 90 days)`;
- the link tray and the `mcp --tray` Cloud link submenu show the entry *Audit trail on — your admin records steps and
  screenshots on this desktop*, and a balloon when it turns on or off (the running link writes the policy into
  `state.json.status.json` as `"trail"`);
- `fairyfly link status` shows `"audit_trail": "on since 2026-10-24 (screenshots: every step; kept 90 days; blurring
  on)"`, or `"off"` (also while the link is not running).

**Screenshots.** The active window of the SAP GUI session the calls use (a popup, if one is open), captured with
`HardCopyToMemory` before and after the step, on the main (COM) thread like the calls themselves. They are PNG, at
most 1920 px wide and 4 MiB (scaled down further if needed). Everything happens in memory: an unblurred screenshot
never reaches a file or the log.

**Blurring** (on by default; the admin can turn it off for bank fields):
- filled with a solid grey before scaling and encoding (not reversible): every password field (`GuiPasswordField`,
  `pwd…` ids, and fields whose name or label is on the password list) always, and while blurring is on every bank
  field: IBAN, account number, bank key, SWIFT/BIC, card numbers, account holder (SAP names such as `BANKN`, `BANKL`,
  `BANKS`, `IBAN`, `SWIFT`, `BKREF`, `KOINH`, `CCNUM`, or labels such as *Kontonr.*, *Kontoinhaber*, *Sort code*);
- the list is libffl's `proto::password_field` / `proto::bank_field`, the same one the gateway uses to mask field
  changes; it is matched on the element id, the SAP field name, the screen label and the tooltip;
- an ALV grid (GuiShell GridView) with such a column is filled as a whole; table-control cells are matched by their
  own ids;
- rectangles come from the elements' `ScreenLeft`/`ScreenTop`/`Width`/`Height`, relative to the window and scaled to
  the captured image, with 3 px around each;
- a screenshot is **dropped** (only the step is sent) when a field that must be blurred cannot be located, when the
  screen's elements cannot be read, or when the image does not have the window's proportions.

- control containers the screen tree does not list (custom controls, docking and splitter containers) are read
  element by element; one that cannot be read is filled as a whole, and so is a grid whose columns cannot be read;
- the image must have the window's proportions (within 2 px) at a display scaling between 0.5 and 4, and the
  window must still be the active one when it is captured (a popup that opened in between is not captured).

Not covered (filled only when the element id, name, label or column name says so): column titles of ALV grids and
table controls with custom (Z) names, classic list output (`lbl[c,r]` lines), HTML viewers and column-tree cells.
Screens that show bank data only there should be kept out of recorded sessions. Calls run one at a time, but with several calls in flight a step's
before/after screenshots can include the effect of another call between them.

**Field changes.** For `gui_element_fill` and the tools annotated `destructiveHint` (e.g. `gui_key_send`,
`gui_element_click`): the target of a fill, read before and after, and every input field on the screen whose value
changed, as `{field: SAP id, label: screen label, old, new}`, at most 200. Values of password fields are always
`••••`, values of bank fields while blurring is on (fairyfly masks them, and libffl and the gateway mask again). A
password field's text is never read.

**Errors** never fail the call: the step is recorded with what could be captured, and the log says why (never with
screen content).

**Testing without the live pairing.** Use your own state file for every manual test (`--state`), and set
`FAIRYFLY_LINK_FORBID_DEFAULT_STATE=1`: then every `link` verb refuses (`STATE_GUARD`) a state in
`%USERPROFILE%\.fairyfly\link\`, so a test copy cannot pause, unpair or overwrite the pairing the person at this
desktop relies on. `link pair` and `link run` print the state they use first (`state: <path>`).
`tests/integration/link_trail_live.ps1` is the live check.

## What runs where

- The tray may start before SAP Logon (at Windows logon): until SAP Logon runs, calls answer that it is not running
  (`gui_doctor`: com_engine "SAP Logon is not running"). Every call looks SAP Logon up again, so once you start it and
  log on the next call sees the session; after SAP Logon was restarted the tray drops the old one by itself. fairyfly
  never starts a SAP GUI scripting engine of its own.
- Nothing has to be attached first: the first call that needs a SAP session attaches to the only open SAP GUI session
  itself (with several open, the assistant is told to pick one or to ask you). After SAP Logon or the tray restarted,
  a closed default connection is re-attached the same way, only to a session of the same SAP system, client and user,
  inside the call you already approved. See "Automatic attach" in [MCP.md](MCP.md).
- Which tools the AI assistant sees is decided by the gateway: your organisation's policy for the user can offer fewer
  tools than this desktop serves (for example only reading tools, without `gui_transaction_start`). Navigation and
  `gui_session_attach` then have to be granted there; the desktop's read-only mode only hides `gui_element_fill`.
- The link offers this desktop's tool list on every connect. The gateway keeps it only from an account in its
  `catalog_publishers` list; otherwise it describes the tools from its own built-in catalog.
- Data entry goes screen by screen: the assistant reads the screen with `only=editable` and fills all its fields in
  one `gui_element_fill` call with `fields`, optionally pressing Enter or F8 afterwards. That is one call and one
  approval per screen (none while **Allow changes in SAP** is on). The gateway must allow writes for you.

- `link run` serves calls through **fairyfly's own MCP dispatcher, in-process**. No local HTTP endpoint and no local
  bearer token are involved.
- The same guards apply as for `fairyfly mcp`, in the same order:
  1. `--read-only` / `FAIRYFLY_READ_ONLY=1` (hard cap);
  2. changes are possible (approval model v2): each needs the gateway's grant and the approval at this desktop;
  3. `--tools` families;
  4. the policy limits.
- Each call additionally carries the gateway's grant. A call the gateway granted read-only runs with fairyfly's
  read-only guard switched on, even when this desktop allows writes.
  Its refusals say which side blocks: the organisation (the fairyfly web app, Settings, "Allow changes in SAP"; only
  an administrator can change it) or this desktop (`--read-only` / `FAIRYFLY_READ_ONLY`), or both.
- A grant limited to transactions (`grants.tcodes`) applies the same rules as `mcp token create --tcode` to every link
  call and batch step: the open transaction must be listed, `gui_menu_select` is refused, `gui_key_send` takes only
  Enter, F4, F8 and the page keys, and a call that ends outside the list blocks screen tools until an allowed
  `gui_transaction_start` (src/include/link/link_call.h). The link announces `tcodes/1`; the gateway sends calls under
  a transaction list to screen tools only to desktops that do, older ones get `DESKTOP_OUTDATED`.
- When the gateway's time limit cancels a call (`cancel/1`), a running batch stops before its next step and still
  answers; its result lists the steps that ran (structuredContent `cancelled`, `items`), and the gateway names them in
  its `CALL_TIMEOUT`.
- A change needs the approval of the person at this desktop (`--approve desktop`, the default; there is no phone
  approval since 2026-10-24, the desktop where SAP GUI runs is the second factor). Approval model v2 (2026-10-25):
  - **Allow changes in SAP** on in the link tray: changes run without asking (the standing approval; also while the
    screen is locked, the person turned it on for when they are away). The decision is `standing`;
  - off (the default):
  - a notification "<assistant> wants to change SAP: VA02 - fill 3 fields, then save" with **Approve**, **Deny** and
    **Details**; it stays on screen until answered and can be answered from the notification centre too;
  - **Details** (or a click on the notification) opens the approval window: the assistant by name ("Claude", the
    client's URL in brackets), the account it acts for, the SAP system, client and transaction, every step numbered
    (standard toolbar buttons named: "click Save (wnd[0]/tbar[0]/btn[11])"), a countdown; **Deny** is the default
    button and Esc;
  - every change asks: a yes approves that change (all steps of that batch) only. The window's **Don't ask again**,
    unticked by default, approves the change and turns on **Allow changes in SAP**;
  - when the organisation requires approval (web settings: "Always ask at the desktop"), the switch and the checkbox
    are off and greyed out ("Approval is required by your administrator"): every change asks, also while the setting
    is on its way to this desktop (the gateway signs it into each call). The saved switch comes back when the
    organisation lets the user decide again;
  - fairyfly started with `--read-only` / `FAIRYFLY_READ_ONLY` asks nothing: it refuses the change itself;
  - keys are named in the steps: "press Save (Ctrl+S)" for `gui_key_send` key 11, "then press Execute (F8)";
  - no answer in 45 s, a locked screen, a disconnected or non-interactive session: denied. Several questions are asked
    one after another, never together;
  - with notifications turned off for fairyfly (or without the Start-menu entry's registration) the window opens at
    once. `--approve no` denies every change. Each decision is in the log (`approval: standing`, `approval <n>:
    approved|denied|expired|locked`). Source: `src/link/desktop_approval.cpp` (decisions), `src/link/desktop_approval_win32.cpp` (UI).
- Every call is written to fairyfly's audit trail with `audit_source: "link"`, the principal `link:<organisation
  id>/<user>`, the AI client and the gateway's request id, so it can be matched with the cloud audit.
- Calls arrive on the link's network threads and are handed to the main (COM) thread one at a time
  (`src/link/main_thread_tools.*`).
- When SAP GUI stops answering a call (a dialog waiting for input, possibly hidden behind other windows; seen live
  with `gui_screen_read` on SU01), every later call waits behind it, because SAP GUI scripting is bound to the main
  thread and fairyfly never makes a second COM call into it from another thread. What the caller gets then:
  - `gui_doctor` and `gui_session_list` do not queue behind it: once the running call is 10 s old (at once, or after
    waiting up to 10 s) they answer `SAP_GUI_NOT_RESPONDING`, a tool result with `isError`, the text "SAP GUI has not
    answered gui_screen_read on connection 0 for 183 s (started 20:27:21); gui_doctor did not run." and what to do,
    and `structuredContent.error` with `code`, `message`, `running_tool`, `connection` ("" when the call named
    none), `elapsed_ms`, `started` (local time) and `hint`. They never run later;
  - any other call that does not start within the gateway's time limit fails with "fairyfly is busy: the call did not
    start within N s; SAP GUI has not answered <tool> on connection <n> for <s> s (started HH:MM:SS)" and the hint;
    once the running call is 60 s old, a new call fails like that at once ("the call did not start; ...") instead of
    holding one of the link's few local call slots, where it would also keep `gui_doctor` out;
  - the hanging call itself, when the gateway's time limit passes, fails with `SAP_GUI_NOT_RESPONDING: SAP GUI has not
    answered ...`; for a tool that changes SAP (destructiveHint: fill, click, key, transaction, batch...) it adds that
    the change may still be applied once SAP answers, so read the screen before retrying it;
  - a call the gateway cancelled (its time limit) while it still waited in the queue never runs later, also when SAP
    GUI answers again: it ends as cancelled, nothing ran;
  - after 60 s the tray turns amber (within one status heartbeat, 15 s) with the menu line "SAP GUI has not answered
    gui_screen_read on connection 0 for 75 s: look for a hidden SAP dialog or a long-running step (e.g. a report
    started with F8), or end that SAP session", a tooltip that names the call and one notice; it turns teal again
    when the call returns (a long report that finishes after 60 s too).

  Answering or closing the SAP dialog, waiting for the long-running step, or ending the SAP session (or SAP GUI)
  releases fairyfly. The log has one
  `SAP_GUI_NOT_RESPONDING` warning per diagnosis.

## The shared protocol library

`third_party/libffl` holds the `ffl/1` frames, call-assertion verification (per-tenant gateway keys pinned at
pairing), pairing, the DPAPI state and the link agent:
- It is vendored from the gateway with `scripts/sync_libffl.py` (see `third_party/libffl/VERSION`), so protocol and
  verification cannot drift between the gateway and fairyfly.
- Never edit the copy here; change the gateway and sync.

## Install and update

- `fairyfly.msi` (per user, no administrator rights): installs to `%LOCALAPPDATA%\Programs\fairyfly`, extends the
  user's `PATH` and adds the Start-menu entry "fairyfly" (the link tray, `link run --tray`; up to 2026.10.23 it was
  "fairyfly (tray)", which an MSI or winget upgrade removes; `update apply` only swaps the exe). The entry starts
  `fairyfly-tray.exe` (no console window). Before replacing or removing the files the installer ends this
  installation's fairyfly (`fairyfly-tray.exe --close <folder>`, the copy inside the MSI): the trays are asked to quit
  as from their menu, and what still runs after 10 s (a link stuck in a SAP call, `fairyfly mcp` for an assistant) is
  terminated, so no reboot is needed and nothing keeps running from removed files. A fairyfly.exe of another folder
  (the Microsoft Store package) is not touched. An uninstall also removes the Run values `fairyfly-link` and
  `fairyfly-mcp` of this folder and the trays' settings (`fairyfly-tray.exe --uninstall <folder>`). After an
  interactive install it starts the tray (`LAUNCHAPP=0` turns that off; never with `/qn`). Source:
  `packaging/fairyfly.wxs`. The CI builds it, installs it on the runner, starts the tray through the shortcut's target
  and `fairyfly mcp`, checks the result and uninstalls it again (the tray must have been asked to quit, nothing of the
  folder may still run, a fairyfly.exe elsewhere must, and the Run value must be gone).
- A tray start that finds its single-instance guard held looks at the holder: this installation's tray opens its menu
  (as before); a tray whose fairyfly.exe no longer exists (left running by an uninstalled MSI up to 2026.10.28) is
  asked to quit and this one starts (after 10 s without success a message names the PID to end in Task Manager); a
  tray of another installation (MSI beside the Store package, another folder) is named in a message and the log
  (`link.log`, `mcp.log`), instead of the start ending silently. The trays also quit on `WM_CLOSE` (`taskkill` without
  `/f`) as from their menu.
- `fairyfly update check|apply`: the update channel is the project's GitHub releases. A release is installed only
  when it is newer, matches `SHA256SUMS` and carries a valid Authenticode signature by the pinned publisher `O=DataZoo GmbH, C=DE`.
  A signed installation never takes an unsigned release; an unsigned installation accepts an unsigned release only
  with `--allow-unsigned` ([SIGNING.md](SIGNING.md#updates)).
- The tray announces a newer release once (`FAIRYFLY_UPDATE_CHECK=0` turns that off).

## Live test

`tests/integration/link_trail_live.ps1` checks the audit trail live with its own state file and test pairing (never
the desktop's real one): the blurring of the current SAP screen (offline), the notice, the status and, through a
gateway with the trail on for that test desktop, the recorded steps. See the script's help.

`tests/integration/link_live.ps1` runs `link run` against a real gateway and a logged-in SAP GUI. It calls the
gateway's MCP endpoint with an access token and checks:
- tools/list and a read call through the gateway;
- the local audit record (`audit_source: "link"`, request id, principal, no screen content);
- pause (`DEVICE_OFFLINE`), resume, and the status after the link ends.

It uses this desktop's real pairing (`%USERPROFILE%\.fairyfly\link\state.json`) and its pause marker:
- Stop a `link run` that is already running first. Otherwise there are two connections for the same device, and the
  test's pause disconnects both.
- It needs an access token in `FFY_ACCESS_TOKEN` and a logged-in SAP GUI session with scripting enabled.
