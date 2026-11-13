# Cloud link (`fairyfly link`)

`fairyfly link` connects this desktop's fairyfly to the fairyfly cloud gateway, so that AI assistants your
organisation has connected there can use your SAP GUI:
- Supported assistants: Gemini Enterprise, Claude, ChatGPT and Copilot Studio.
- The connection is outbound only: no port is opened on the PC and no VPN is needed. It goes through the corporate
  proxy (found automatically from the browser policies and Windows settings, or set with `fairyfly proxy set`;
  [Corporate proxies](#corporate-proxies)).
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

- The tray may start before SAP Logon. Session startup is on demand: configure `fairyfly session default "Bigfox"`,
  enable **Allow automatic SAP startup** locally, and have the gateway administrator permit startup in Settings.
  A headless `link run` uses `--allow-sap-startup` instead of the tray checkbox. If a requested task needs a session,
  Fairyfly can start SAP Logon, open the default connection and log in through local credentials or SSO. Disabled
  settings produce actionable MCP errors. An existing authenticated session remains usable without this permission.
  Startup permission is independent of **Allow changes in SAP**. See [On-demand startup](MCP.md#on-demand-startup).
- Nothing has to be attached first: the first call that needs a SAP session attaches to the only open SAP GUI session
  itself (with several open, the assistant is told to pick one or to ask you). After SAP Logon or the tray restarted,
  a closed default connection is re-attached the same way, only to a session of the same SAP system, client and user,
  inside the call you already approved. See "Automatic attach" in [MCP.md](MCP.md).
- Which tools the AI assistant sees is decided by the gateway: your organisation's policy for the user can offer fewer
  tools than this desktop serves (for example only reading tools, without `gui_transaction_start`). Navigation and
  `gui_session_attach` then have to be granted there; the desktop's read-only mode only hides `gui_element_fill`.
- The link offers this desktop's tool list on every connect. The gateway keeps it only from an account in its
  `catalog_publishers` list; otherwise it describes the tools from its own built-in catalog.
- Data entry goes screen by screen: the assistant reads the screen with `only=editable` and fills the requested fields in
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
  - a notification "<assistant> wants to change SAP: VA02 - fill 3 fields (Order Type, Sales Organization,
    Distribution Channel), then press Save (Ctrl+S)" with **Approve**, **Deny** and **Details**; filled fields are
    named by their screen label (read from the active window), else by the field name without its type prefix
    ("SE16 - fill DATABROWSE-TABLENAME, then press Enter"), never by the element id (labels are cleaned of control,
    bidi and zero-width characters and cut to 40 bytes; the window's steps add the name: "Table Name
    (DATABROWSE-TABLENAME)"); it stays on screen until answered
    and can be answered from the notification centre too;
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

## Corporate proxies

The link, `link pair` and `fairyfly update` go through the proxy your organisation uses. Without any setting fairyfly
finds it itself, from the same places Edge, Chrome and Firefox and Windows take it; you or your IT can also set it for
fairyfly. Source: `src/link/proxy_config.cpp` (the sources and their order, portable), `src/link/proxy_sources_win32.cpp`
(registry, policies.json), `src/link/system_proxy*.cpp` (Windows settings, WinHTTP, SSPI), libffl's `proxy.*` (the
tunnel).

**Which proxy.** The first of these that decides wins:

| # | Source | Where | Decides when |
|---|---|---|---|
| 1 | **fairyfly policy** (IT) | `HKLM\SOFTWARE\Policies\DataZoo\fairyfly`: `ProxyMode` (`auto`, `manual`, `pac`, `direct`), `ProxyServer` (`host:port`), `ProxyBypass`, `ProxyPacUrl` (REG_SZ) | `ProxyMode` is not `auto`. The setting is **locked**: the tray window shows "Set by your administrator", `fairyfly proxy set/reset` answer `PROXY_SETTING_LOCKED` |
| 2 | **your fairyfly setting** | `fairyfly proxy set`, or the link tray's **Network / proxy...** (per user, `HKCU\Software\DataZoo\fairyfly\tray`, values `proxy_mode`, `proxy_server`, `proxy_bypass`, `proxy_pac_url`) | its mode is not Auto (and no fairyfly policy exists) |
| 3 | **`HTTPS_PROXY`** (and `NO_PROXY`) | the environment of the fairyfly process | it is set |
| 4 | **browser policy** | Edge, then Chrome: `HKLM`, else `HKCU` `\Software\Policies\Microsoft\Edge` / `\Google\Chrome` (`ProxyMode`, `ProxyServer`, `ProxyPacUrl`, `ProxyBypassList`, the `ProxySettings` JSON policy, legacy `ProxyServerMode`); then Firefox: `HKLM\Software\Policies\Mozilla\Firefox\Proxy` (`Mode`, `HTTPProxy`/`SSLProxy`, `UseHTTPProxyForAllProtocols`, `AutoConfigURL`, `Passthrough`), else `distribution\policies.json` in Firefox's install folder | the first browser with a proxy policy decides; a policy that says "use the system settings" (`system`) hands over to 5 |
| 5 | **Windows** | the user's setup script (PAC), then "Automatically detect settings" (WPAD), then the manual proxy server, then `netsh winhttp` | as before (below) |
| | otherwise | | direct |

Modes of 1 and 2: **Auto** (decides nothing: 3 to 5 decide; a fairyfly policy in Auto also locks your setting to
Auto), **Manual** (`host:port` plus exceptions such as `<local>;*.corp.example;10.*`), **PAC** (the script's address,
`http://` or `https://`) and **Direct** (no proxy, whatever 3 to 5 say). A fairyfly policy with an invalid value
still locks; the proxy is then chosen as in Auto, and `fairyfly proxy show` and `fairyfly doctor` say what is wrong.
`NO_PROXY` belongs to 3: it does not override 1 or 2, which have their own exceptions.

Why browser policy ranks above the Windows settings: an administrator who sets a browser's proxy by policy names the
proxy explicitly for this desktop's traffic, while the Windows settings are often the user's own (Internet Options
can be changed without rights) or empty where the browsers are managed. A browser policy that defers to the system
(`ProxyMode` `system`, Firefox `Mode` `system`) keeps the Windows settings in charge.

Windows settings (5), in the order Windows uses them:
1. **Use setup script** (a PAC file at the address IT configured), then **Automatically detect settings** (WPAD) if
   there is no script or it fails: WinHTTP evaluates them for the gateway's address (`https://<gateway>/…`; the link's
   `wss://` is asked as `https://`). The answer is followed in its order: `PROXY a:8080; PROXY b:8080; DIRECT` tries
   a, then b (only when a cannot be reached), then connects directly. A proxy that answers (even with an error) is not
   skipped.
2. If neither is on, or both fail: the **manual proxy server** (`https=` entry, else the one for
   all protocols) with its exceptions ("Don't use the proxy server for local addresses" = `<local>`, `*.corp.example`,
   `10.*`).
3. Otherwise WinHTTP's machine setting (`netsh winhttp set proxy`).
4. Otherwise direct.

A PAC script named by 1, 2 or a browser policy that cannot be downloaded or evaluated means **direct** for that
source (as Chrome does without `ProxyPacMandatory`), with the reason in `fairyfly proxy show`; the sources below it
are not tried.

**Setting it yourself.**

| Command | What it does |
|---|---|
| `fairyfly proxy show` | the setting (and whether it is locked), the route in plain words, where it came from, its trust, and what each source says |
| `fairyfly proxy set --mode manual --server proxy.corp.example:8080 [--bypass "<local>;*.corp.example"]` | your Manual setting |
| `fairyfly proxy set --mode pac --pac https://pac.corp.example/proxy.pac` | your PAC setting |
| `fairyfly proxy set --mode direct` / `--mode auto` | Direct / back to automatic |
| `fairyfly proxy test [--mode ... --server ...]` | a real connection through the route: HTTPS `GET /health` on the gateway, then the link's WebSocket upgrade (closed at once, nothing sent); with `--mode` it tests that setting without saving it. `PROXY_TEST_FAILED` with the error otherwise |
| `fairyfly proxy reset` | forgets your setting (Auto) |

The link tray's **Network / proxy...** opens a window with the same: the mode, its fields, the route in plain words
("via proxy.corp.example:8080 from PAC (https://pac.corp.example/proxy.pac, Edge policy (HKLM)), Windows login: yes",
or "direct (your fairyfly setting: Direct)"), **Test connection** (the same test, with what the window shows, saved or
not), **Save** and **Cancel**; every field is locked when the fairyfly policy applies. A saved change takes effect at
once: the running link notices within a second (also one changed by `fairyfly proxy set` in a console, or by a new
policy), drops the cached routes and reconnects along the new route; no restart. `fairyfly doctor` has a `proxy`
check: setting, route, source, trust and the connection test (warn `PROXY_TEST_FAILED` when the gateway cannot be
reached).

What is checked before a setting is saved or used (the same for the fairyfly policy's values): the proxy is `host:port`
(a leading `http://` is accepted); `https://` proxy addresses (TLS to the proxy), `socks://` and other schemes,
`user:password@`, paths, lists, spaces and control characters (line breaks) are refused; the PAC address is `http://`
or `https://` with a host name; exceptions are printable ASCII. Credentials never go into a setting: the Windows login
is used, or a Credential Manager entry (below).

Results are kept for 5 minutes (emptied at once when the fairyfly policy or your setting changes). The link log
(`link.log`) has one line per gateway host when the choice changes, e.g.
`proxy: fairyfly.io -> via proxy.corp.example:8080 from PAC (http://pac.corp.example/proxy.pac, Windows proxy settings), Windows login: yes`,
and the link connects with `link: connecting through proxy proxy.corp.example:8080 (from the proxy settings (fairyfly proxy show))`.

**How it connects.** Through a proxy, fairyfly sends `CONNECT <gateway>:443` and runs TLS inside the tunnel, end to end
with the gateway: the certificate and host name are checked as on a direct connection, and the proxy sees only the
host name and port. The WebSocket of the link and every HTTPS request take the same path. Reconnecting works as
without a proxy (backoff up to 60 s); a proxy that goes away and comes back is used again.

**Proxy authentication.** Which credentials a proxy gets depends on where it came from:

| Proxy from | Windows login (Negotiate/NTLM) | Basic (stored credential) |
|---|---|---|
| named explicitly: the fairyfly policy or your fairyfly setting in Manual, `HTTPS_PROXY`, a browser policy's proxy server, the manual proxy settings, `netsh winhttp` / group policy machine setting | yes | yes |
| a **trusted** PAC script: its address is `https://`, or it is set by an administrator with a dotted host name (`http://pac.corp.example/proxy.pac`): the fairyfly policy, an Edge/Chrome policy, a Firefox policy only administrators can change, the Windows "automatic configuration script" group policy | yes | no |
| any other PAC script (`http://` set by the user, in fairyfly or in Windows; a Firefox policies.json that a non-administrator can change; a single-label host like `http://wpad/`; an IP address) | no, as for WPAD | no |
| automatic detection (WPAD), also when a browser policy turns it on | **no**, unless the administrator allows it (below) | no |

A browser policy counts as an administrator's when only administrators can change it: the registry policy keys
always; Firefox's `policies.json` only when the file, its `distribution` folder and the install folder are owned by
SYSTEM, Administrators or TrustedInstaller and nobody else may write, delete or re-permission them (a per-user Firefox
install, or a folder with "Users: Modify", makes it the user's own setting).

- **Negotiate (Kerberos) or NTLM with the Windows login** of the user running fairyfly, through SSPI. Nothing has to be
  configured and no password is stored or asked for. Negotiate is preferred when the proxy offers both; the SPN is
  `HTTP/<proxy host>` as written in the settings.
- **Not to WPAD-discovered proxies.** Anyone on the local network can answer WPAD's discovery and name themselves as
  the proxy; sending them the Windows login would hand over the user's NTLM response (crackable offline, relayable).
  Such a proxy that asks for the login gets nothing, and the log says `the proxy requires the Windows login (NTLM)
  but was discovered automatically (WPAD): not sending credentials; ask IT to set the proxy explicitly`. IT should set
  the proxy explicitly (manual setting, `netsh winhttp`, or a PAC script at a fixed address). Where WPAD is controlled
  (DHCP/DNS of the domain only), an administrator can allow it: `HKLM\SOFTWARE\Policies\DataZoo\fairyfly`, DWORD
  `AllowLoginToDiscoveredProxies` = `1` (read at each proxy connection).
- **Downloading the script.** When the server of a proxy script asks for a login (401), WinHTTP answers with the
  Windows login only for a trusted PAC script (above) or with the administrator's WPAD policy; WinHTTP would otherwise
  treat a single-label host such as `wpad` as intranet and send NTLM to whoever answers that name. A configured script
  that fails and falls back to automatic detection is logged as a warning.
- Group policy here means the "automatic configuration script" policy (`AutoConfigURL` under
  `HKLM` or `HKCU\Software\Policies\Microsoft\Windows\CurrentVersion\Internet Settings`) holding the same address.
- **NTLMv1**: whether Windows sends NTLMv2 or the weak NTLMv1 is the machine's `LmCompatibilityLevel` (group policy
  "Network security: LAN Manager authentication level"); it should be "Send NTLMv2 response only. Refuse LM & NTLM"
  (5). fairyfly does not change it.
- **Basic** only to a proxy set explicitly, only when it asks for Basic, and only with a credential stored for that
  proxy in the Windows Credential Manager, as a generic credential named `fairyfly-proxy:<proxy host>`:
  `cmdkey /generic:fairyfly-proxy:proxy.corp.example /user:CORP\alice /pass` (prompts for the password). The credential
  is read only when the proxy's 407 offers Basic. Basic sends the password to the proxy unencrypted (base64); prefer
  Negotiate.
- Credentials are never written to the log or an error message, and never put into a URL.

**Environment.** `HTTPS_PROXY=http://proxy.corp.example:8080` uses that proxy for `https://` and `wss://`
instead of the browser policies and the Windows settings (not instead of the fairyfly policy or your fairyfly setting;
fairyfly takes both variables out of libffl's view at start and ranks them itself, child processes still inherit
them) (with `http://user:password@host:port` its user and password are offered as Basic;
prefer the Credential Manager entry). `NO_PROXY=fairyfly.io,.corp.example` connects directly to those hosts (exact names,
`.suffix`, `*.suffix`, `*`), also when a browser policy or the Windows settings name a proxy. Loopback (`localhost`, `127.*`, `[::1]`) is
never proxied. `https://proxy` addresses (TLS to the proxy) and SOCKS are not supported: such an entry is skipped
with a warning in the log (and the next entry, or DIRECT if the settings list it, is used).

**Errors** (in `link run`, the tray's notices and `link.log`; the link keeps retrying):

| Message | Meaning |
|---|---|
| `proxy refused CONNECT 403 Forbidden (proxy:8080 -> fairyfly.io:443, proxy from the proxy settings (fairyfly proxy show))` | the proxy's policy blocks the gateway: IT must allow it (below) |
| `proxy requires authentication (offers NTLM, Negotiate), and the proxy requires the Windows login (NTLM) but was discovered automatically (WPAD): ...` | the proxy came from WPAD: IT sets it explicitly, or allows the login by policy (above) |
| `proxy requires authentication (offers Basic), and no credentials are configured for it` | only Basic is offered: store a `fairyfly-proxy:<host>` credential, or ask IT for Negotiate/NTLM |
| `proxy requires authentication: it refused the NTLM credentials of this user` | the Windows login was rejected (user not allowed through, or the proxy is not in the domain) |
| `proxy closed the connection during NTLM authentication` | the proxy does not keep the connection open for NTLM (needs HTTP keep-alive) |
| `cannot reach proxy proxy:8080 (...)` | no proxy in the list could be reached (and the settings list no `DIRECT`) |

**What IT must allow** (on the proxy and the firewall):
- `CONNECT` to port **443** of the gateway's host (`fairyfly.io`, or your gateway) and of the link host it hands out
  at pairing (the `wss://` address), plus `github.com` and `objects.githubusercontent.com` (release downloads) for
  `fairyfly update`.
- **No TLS inspection** (break and inspect) for these hosts: fairyfly checks the gateway's certificate against the
  Windows certificate store, so an inspecting proxy works only if its CA is trusted there and it passes the link's
  long-lived WebSocket (`Upgrade: websocket`) unchanged. Exempting the hosts is simpler.
- Long-lived connections: the link holds one WebSocket open and sends a ping every 20 s; allow tunnels to stay idle
  for at least 60 s.
- Authentication by Negotiate/NTLM with the user's domain login (connection-based: keep-alive on the proxy
  connection), or Basic with a stored credential.

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
