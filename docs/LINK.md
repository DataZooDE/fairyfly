# Cloud link (`fairyfly link`)

`fairyfly link` connects this desktop's fairyfly to the fairyfly cloud gateway, so that AI assistants your
organisation has connected there can use your SAP GUI:
- Supported assistants: Gemini Enterprise, Claude, ChatGPT and Copilot Studio.
- The connection is outbound only: no port is opened on the PC and no VPN is needed.
- The cloud side is described in the gateway's documentation (research repository,
  `2026-10-14-fairyfly-cloud-agent-access/gateway`).

| Command | What it does |
|---|---|
| `fairyfly link pair [--gateway https://app.example]` | Shows a QR code and a number. Scan the code with your phone, sign in, type the number. The desktop then shows **which account and organisation** it would act for (and whether the identity provider verified the e-mail address); type `yes` only if that is you |
| `fairyfly link status` | Paired or not, with device, account, organisation and gateway |
| `fairyfly link run [--allow-write] [--tools session,screen] [--approve desktop\|no]` | Connects and serves tool calls until Ctrl+C |
| `fairyfly link run --tray [--install-autostart\|--remove-autostart]` | The same link in the system tray, without the MCP server (see [Tray](#tray)) |
| `fairyfly link pause` | Disconnects a running link until resumed: the AI assistant cannot reach this desktop meanwhile (the gateway answers `DEVICE_OFFLINE`). Works from any console; the running link follows within a second |
| `fairyfly link resume` | Lets a paused link connect again |
| `fairyfly link unpair` | Forgets the pairing here (remove the desktop in the web app too) |

Without `--gateway`, `link pair` uses `FAIRYFLY_LINK_GATEWAY`, and otherwise asks for the address.

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

Files, all in `%LOCALAPPDATA%\fairyfly\link\`:
- `state.json`: the pairing. The device key inside it is protected with DPAPI.
- `state.json.paused`: exists while the link is paused.
- `state.json.status.json`: written by the running link (`connected`, `paused`, `stopped`, with its process id and
  time). `link status` and the tray read it; a status older than 60 s means the link is not running.

## Tray

`fairyfly link run --tray` runs the link in the system tray. It has its own icon and does not start the MCP server
or an HTTP endpoint.
- It detaches from the console, like `mcp --tray`. Only one link tray runs per Windows session.
- A desktop that is not paired is refused in the console with `LINK_NOT_PAIRED`; pair it first.
- The icon is green while connected, yellow while paused and grey otherwise.
- A notice appears when the link starts, pauses, resumes or stops.
- The menu has:
  - the pairing (organisation, account) and the link's state;
  - **Pause** / **Resume**;
  - **Allow changes in SAP** (a check mark): switches the running link between read-only and write mode without a
    restart. Turning it on asks first. The link then reconnects and offers its new tool list again. The switch is
    audited (`read_only` / `write_mode`). It is off at start unless `--allow-write` was given, and it is locked by
    `--read-only` / `FAIRYFLY_READ_ONLY=1`. Every change still needs the gateway's grant and an approval;
  - **Health check**: `link status` in a console;
  - **Pair again**: `link pair` in a console (pairing needs the person at the desktop);
  - **Open log** (`%LOCALAPPDATA%\fairyfly\logs\link.log`), **Open audit trail folder**, **Check for updates**;
  - **Quit**, which disconnects the link and ends the process.
- The other `link run` options apply as usual: `fairyfly link run --tray --allow-write --tools session,screen`.
- If the link ends with an error, a message box points to the log.

Autostart at logon:

    fairyfly link run --tray --install-autostart [--allow-write] [--tools ...]
    fairyfly link run --tray --remove-autostart

This writes or removes the value `fairyfly-link` under `HKCU\Software\Microsoft\Windows\CurrentVersion\Run`. The
value holds the `link run --tray` command with the options given, without the autostart flag. It is independent
of the MCP tray's `fairyfly-mcp` value.

`fairyfly mcp --tray` also has a **Cloud link** submenu:
- the pairing (organisation, account) and the link's state;
- **Connect to cloud...** or **Pair again**: `link pair` in a console (pairing needs the person at the desktop);
- **Start link**: starts the link tray (`fairyfly link run --tray`);
- **Pause** / **Resume**;
- **Health check**: `link status` in a console.

## What runs where

- Nothing has to be attached first: the first call that needs a SAP session attaches to the only open SAP GUI session
  itself (with several open, the assistant is told to pick one or to ask you). See "Automatic attach" in
  [MCP.md](MCP.md).
- Which tools the AI assistant sees is decided by the gateway: your organisation's policy for the user can offer fewer
  tools than this desktop serves (for example only reading tools, without `gui_transaction_start`). Navigation and
  `gui_session_attach` then have to be granted there; the desktop's read-only mode only hides `gui_element_fill`.
- The link offers this desktop's tool list on every connect. The gateway keeps it only from an account in its
  `catalog_publishers` list; otherwise it describes the tools from its own built-in catalog.
- Data entry goes screen by screen: the assistant reads the screen with `only=editable` and fills all its fields in
  one `gui_element_fill` call with `fields`, optionally pressing Enter or F8 afterwards. That is one call and one
  approval per screen. It needs write access on both sides: the gateway must allow writes for you, and
  **Allow changes in SAP** must be on in the link tray.

- `link run` serves calls through **fairyfly's own MCP dispatcher, in-process**. No local HTTP endpoint and no local
  bearer token are involved.
- The same guards apply as for `fairyfly mcp`, in the same order:
  1. `--read-only` / `FAIRYFLY_READ_ONLY=1` (hard cap);
  2. read-only unless `--allow-write` (or **Allow changes in SAP** in the link tray);
  3. `--tools` families;
  4. the policy limits.
- Each call additionally carries the gateway's grant. A call the gateway granted read-only runs with fairyfly's
  read-only guard switched on, even when this desktop allows writes.
- A change needs the person's approval:
  - on this desktop (a dialog, `--approve desktop`, the default), or
  - with a passkey on the phone (the gateway puts the approval into the signed call).
- Every call is written to fairyfly's audit trail with `audit_source: "link"`, the principal `link:<organisation
  id>/<user>`, the AI client and the gateway's request id, so it can be matched with the cloud audit.
- Calls arrive on the link's network threads and are handed to the main (COM) thread one at a time
  (`src/link/main_thread_tools.*`).

## The shared protocol library

`third_party/libffl` holds the `ffl/1` frames, call-assertion verification (per-tenant gateway keys pinned at
pairing), pairing, the DPAPI state and the link agent:
- It is vendored from the gateway with `scripts/sync_libffl.py` (see `third_party/libffl/VERSION`), so protocol and
  verification cannot drift between the gateway and fairyfly.
- Never edit the copy here; change the gateway and sync.

## Install and update

- `fairyfly.msi` (per user, no administrator rights): installs to `%LOCALAPPDATA%\Programs\fairyfly`, extends the
  user's `PATH` and adds the Start-menu entry "fairyfly (tray)". Source: `packaging/fairyfly.wxs`. The CI builds it,
  installs it on the runner, checks the result and uninstalls it again.
- `fairyfly update check|apply`: the update channel is the project's GitHub releases. A release is installed only
  when it is newer, matches `SHA256SUMS` and carries a valid Authenticode signature by the same signer as the
  installed one. An unsigned self-built installation accepts an unsigned release only with `--allow-unsigned`.
- The tray announces a newer release once (`FAIRYFLY_UPDATE_CHECK=0` turns that off).

## Live test

`tests/integration/link_live.ps1` runs `link run` against a real gateway and a logged-in SAP GUI. It calls the
gateway's MCP endpoint with an access token and checks:
- tools/list and a read call through the gateway;
- the local audit record (`audit_source: "link"`, request id, principal, no screen content);
- pause (`DEVICE_OFFLINE`), resume, and the status after the link ends.

It uses this desktop's real pairing (`%LOCALAPPDATA%\fairyfly\link\state.json`) and its pause marker:
- Stop a `link run` that is already running first. Otherwise there are two connections for the same device, and the
  test's pause disconnects both.
- It needs an access token in `FFY_ACCESS_TOKEN` and a logged-in SAP GUI session with scripting enabled.
