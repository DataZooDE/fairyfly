# Changelog

## 2026.10.27 (2026-10-27)

### Releases: code signing with Microsoft Artifact Signing (ready, off until configured)

- `release.yml` signs with `azure/artifact-signing-action` v2.0.0 (pinned to its commit) after an OIDC `azure/login`:
  no client secret, the Entra federated credential is bound to the environment `release-signing`. It signs every exe
  of the build (`fairyfly.exe`, `fairyfly-tray.exe`), builds the MSI around the signed exes and signs the MSI, all
  with RFC 3161 time stamps. The winget manifests and `SHA256SUMS` are made from the signed files, and the release job
  checks that what it publishes is what was verified.
- Signing is switched on by the repository **variable** `AZURE_SIGNING_ACCOUNT` (with `AZURE_SIGNING_PROFILE`,
  `_ENDPOINT`, `_CLIENT_ID`, `_TENANT_ID`, `_SUBSCRIPTION_ID`; a partial set fails the release), no longer by the secret
  `AZURE_SIGNING_CLIENT_SECRET`. Without it the release is unsigned as before; `REQUIRE_SIGNING=true` makes that an
  error. The one-time Azure and GitHub setup is in [docs/SIGNING.md](docs/SIGNING.md).
- New `packaging/sign/Test-Signature.ps1` checks every signed file and fails the release unless the embedded
  signature is valid and trusted (`Get-AuthenticodeSignature`, `signtool verify /pa`), time-stamped, and made by the
  updater's pin (exactly one `O=DataZoo GmbH` and one `C=DE`, read from `CMakeLists.txt`). Before, only the exes were
  checked, with `signtool verify /pa` alone (any trusted signer, no time stamp needed), and the MSI not at all. Its
  tests (`signing.yml`, no secrets) refuse an unsigned file, a self-signed certificate, a missing time stamp and a
  wrong `O=`/`C=`, and accept a valid signature; the workflow also runs actionlint.
- Only the tagged commit is built and signed: the manual run has no tag input any more (`gh workflow run release.yml
  --ref vYYYY.MM.DD`), the build and sign jobs refuse a run whose `github.ref` is not `refs/tags/<tag>`, and every job
  checks out `github.sha`. All actions in `release.yml` are pinned to commit SHAs, and WiX is installed by
  `packaging/sign/Install-Wix.ps1` from a SHA-512-pinned package, before the Azure login. `Test-ReleaseWorkflow.ps1`
  (signing.yml) guards these rules.
- docs/SIGNING.md: the Entra credential is a flexible federated credential bound to `release.yml` on `v*` tags
  (`job_workflow_ref`), the `release-signing` environment needs a required reviewer, and a tag ruleset (the `gh api`
  command is documented, not applied) reserves `v*` tags to admins.

### winget: manifests on schema 1.12.0, with PrivacyUrl and the copyright years

- The manifests of `DataZoo.fairyfly` (packaging/winget/templates) use manifest schema 1.12.0, the version
  microsoft/winget-pkgs recommends (1.10.0 is the oldest it still accepts), add `PrivacyUrl` (the privacy statement in
  docs/SECURITY.md) and carry the exe's copyright, `Copyright (c) 2025-2026 DataZoo GmbH`.
- `packaging/winget/tests/Test-WingetTemplates.ps1` (any pwsh 7, run by winget.yml) checks the templates against the
  winget-pkgs rules: schema header and version, required fields, ShortDescription and tag limits, https URLs,
  `PrivacyUrl`, no `Agreements`, Publisher/PackageName/Copyright equal to the MSI and the exe.

### CommandHandler split; doctor checks testable; explicit result rendering (2026-10-25 code review)

- `cli_handler.cpp` (2,815 lines) is split: screen filters (`screen_filter.cpp`), the doctor
  (`cli_handler_doctor.cpp` on top of `doctor_checks.cpp`: Win32 probes plus pure, unit-tested judges) and output
  formatting (`output_format.cpp`).
- How a result renders as Markdown is part of its command's row in the command table (`screen read`,
  `screen find`, `doctor`); `format_output` no longer guesses it from the keys of the result.

### Element metadata state per screen read (2026-10-25 code review)

- The table/tree cache and the `--skip-trees` option of the element metadata extractor belong to one screen read
  instead of being process-wide statics that every reader had to reset by hand.

### CommandHandler gets its engine injected (2026-10-25 code review)

- `CommandHandler` takes its automation engine and connection store from outside (the default constructor still
  builds the real ones), and asks the engine for SAP GUI scripting (`SapGuiScripting`: session, keys, popups,
  menus, grid cells, read-only peeks) instead of 23 downcasts to the COM engine. A fake engine now tests the
  handlers without SAP GUI: read-only refusals before SAP is asked, connection resolution, error passing, keys and
  popups.

### One spelling of a SAP identity (2026-10-25 code review)

- "SID/CLIENT/USER" is built by `sap_identity()` / `SessionTarget::identity()` and looked up by `identity_listed()`
  instead of 15 hand-written concatenations and two separate matchers; `RateLimiter` is one budget of the keyed
  limiter instead of a second sliding-window implementation.

### One table for the mcp.yaml keys (2026-10-25 code review)

- Each key of `mcp.yaml` names its `fairyfly mcp` options and the server option it sets in the key table; the flag
  map and `apply_config` are derived from it instead of being two more hand-kept tables. The precedence
  (flag > environment > `mcp.yaml` > default) is implemented once (`resolve_layers`); `merged_layers` and the audit
  settings use it.

### Tool classification in the command table (2026-10-25 code review)

- What a tool is (global control, targetless, listing, session system, lease-capable, worker-bound, screen-bound,
  acts on the open transaction, returns screen data) is a trait of its row in `command_table.cpp`. Routing,
  authorization, leases, the session worker, owner-mode batches and the untrusted-data header read it there
  instead of nine hand-written lists in seven files.

### Tests: shared support, no fixed sleeps, faster (2026-10-25 code review)

- `tests/unit/support/`: `TempDir` (removed also when a test fails; replaces five helpers and 12 inline
  directories that were left behind on a failure), `eventually`, `spec_of`, `EnvGuard`; tests use `Result::ok`.
- Waiting on a condition instead of fixed sleeps in the HTTP queue, disconnect and executor-pool tests; the worker
  fixture's slow call takes 600 ms instead of 2 s. A unit run takes about 40 s (84 s before this review).
- Tests that asserted nothing are gone; the redaction tests moved to `test_sensitive_data.cpp`, the read-only
  guard tests to `test_read_only_guard.cpp`.

### One definition for shared constants and command-line quoting (2026-10-25 code review)

- The default ports (8383, 8443), the protocol versions (one table that says which are spoken over stdio and over
  HTTP), the bearer challenge and the `ffy_` token prefix are each defined once (`mcp/constants.h`,
  `auth/token_store.h`); help texts and the server banner are built from them.
- Windows command-line quoting is one header (`text/command_line.h`) for the SAP shortcut launcher, the tray and
  fairyfly-tray.exe, tested against `CommandLineToArgvW`.
- HTTP failures outside JSON-RPC are built by one function: the transport's ones are now JSON-escaped, and every
  one carries `Cache-Control: no-store`.

### One way to build an error result (2026-10-25 code review)

- `Result::fail(code, message[, suggestions])` and `Result::set_error(...)` replace about 230 hand-built error
  results (status, code, message, suggestions set field by field) and seven private copies of the same factory.
  `set_error` keeps a result's data, diagnostics, duration and extra error fields, so outputs are unchanged.

### Shared string and UTF-8 helpers (2026-10-25 code review)

- One `lower_ascii` / `upper_ascii` / `trim` / `iequals` / `is_loopback_host` (`string_utils.h`) instead of about
  25 private copies; ASCII only, independent of the C locale.
- One UTF-8 <-> UTF-16 module (`text/utf8.h`) replaces nine hand-written converters, with two explicit policies.
  Strict (throws) for everything a decision or an identity depends on: SAP strings, credential targets, user
  names and passwords, setup's account names, URL prefixes, SDDL and the elevated command line, PowerShell
  parameters, the tray's command lines, environment, registry and mutex names. Lossy (never throws, U+FFFD) only
  for text people read: logs, window titles, tooltips, balloons, menus, console output.
- Malformed UTF-8 in a connection name, user name or password is now refused (`INVALID_CONNECTION_NAME`,
  `INVALID_ARGUMENT`, `INVALID_PASSWORD`) instead of being replaced. Before, two malformed names could share one
  Credential Manager entry, and a password could change before it reached SAP.
- COM member names in log lines are UTF-8 now (they went through the ANSI code page). Tray tooltips no longer
  throw on a UTF-8 sequence cut at the length limit.

### Dead code removed (2026-10-25 code review)

- About 730 lines that nothing called: the old copy of the element metadata extractor in the automation engine
  (the live one is `ElementMetadataExtractor`), `TreeFormatter`, the no-op `ConfigManager`/`SessionManager` stubs,
  `retry.h`, unused free COM helpers, a placeholder tree-text helper, and `name()`/`description()` on every
  command (help comes from the command table). `CommandBase` owns the subcommand pointer and the default
  `was_invoked()` that 17 commands repeated. Tests that duplicated others are gone.

### Review fixes (2026-10-25 code review)

- `mcp.yaml`'s `audit.enabled`, `audit.required` and `audit.file` (and `FAIRYFLY_MCP_AUDIT_*`) are applied to the MCP
  server (stdio, HTTP and tray). They were validated and documented but ignored, so `audit.required: true` did
  nothing. On/off and the file: flag > environment > `mcp.yaml` > default.
- "Required" is monotonic: once `--audit-required`, `FAIRYFLY_AUDIT=required`, `audit.required: true` or
  `FAIRYFLY_MCP_AUDIT_REQUIRED=true` asks for it, no disable switches it off. Before, `--no-audit` beat
  `FAIRYFLY_AUDIT=required`.
- `auth.token_prefix` is removed (it was never applied; tokens always start with `ffy_`). A file that still has it
  gets the usual "unknown key (removed: ...)" warning.
- A result no command set reports `INTERNAL_NO_STATUS` instead of "not implemented … Phase 1 scaffolding".
- The unit tests that attach to a live SAP GUI session (and could select tabs in it) are tagged `[.][live]` and no
  longer run by default; the two slow http.sys timeout tests are tagged `[.][slow]`. The unit run takes about 48 s
  instead of 84 s.

### One reader for environment variables

- `FAIRYFLY_READ_ONLY` and the other switches are read by one shared reader (`config::Environment`). They are on
  for `1`, `true`, `yes` or `on` in any case, everywhere. Before, `mcp config` and `mcp setup` accepted only some
  spellings: with `FAIRYFLY_READ_ONLY=Yes` the MCP server refused changes while `mcp setup` would change the
  machine and `mcp config` reported no hard cap.
- `FAIRYFLY_DIAG` and `FAIRYFLY_PROBE_ALL` follow the same rule (they took only `1`). `FAIRYFLY_AUDIT` and
  `FAIRYFLY_UPDATE_CHECK` are switched off by `0`, `false`, `no` or `off` in any case (`OFF` or `no` left the tray's
  update check on). `mcp.yaml` booleans use the same spellings.
- The cloud link and its tray read them the same way too (`FAIRYFLY_READ_ONLY`, `FAIRYFLY_LINK_GATEWAY`,
  `FAIRYFLY_LINK_FORBID_DEFAULT_STATE`).

### `fairyfly update apply`: the publisher is pinned (security)

- A release must be signed by `O=DataZoo GmbH, C=DE` (organisation and country of the signer's certificate, compared
  normalised, not the whole subject). Until now an unsigned installation accepted an executable validly signed by
  *any* publisher whose `SHA256SUMS` in the same release matched, and a signed installation compared the full
  certificate subject, so a new address in a re-validated certificate would have blocked every update. A signer
  other than the pinned one is refused with `UPDATE_SIGNER_UNEXPECTED` (it replaces `UPDATE_SIGNER_CHANGED`).
- A signature that is present but does not verify (expired without a time stamp, revoked, tampered) is refused with
  `UPDATE_SIGNATURE_INVALID` also with `--allow-unsigned`; it was taken as "unsigned" before.
- Unchanged: `SHA256SUMS` is mandatory, a signed installation never takes an unsigned release, and an unsigned
  installation (the releases up to 2026.10.x are unsigned) takes an unsigned release only with `--allow-unsigned`; the
  result now carries a `warning` that nothing proves who built it.
- Forks set their own publisher with the CMake options `FAIRYFLY_UPDATE_SIGNER_ORGANIZATION` and
  `FAIRYFLY_UPDATE_SIGNER_COUNTRY` ([docs/SIGNING.md](docs/SIGNING.md#updates)).

### Cloud link and stdio MCP: re-attach without a verified SAP user only for reads

- A closed default connection is re-attached for a change (a write or destructive tool such as `gui_element_click`,
  `gui_key_send`, `gui_element_fill`, `gui_batch`) only when the only open SAP session has the same SAP system,
  client and logged-on user that the saved connection recorded.
- A connection file of an older version (no system, client or user recorded), one saved before anybody logged on,
  or an open session where nobody is logged on (the SAP logon screen) is re-attached for read-only tools only; the
  note in the result says so. A change keeps `INVALID_CONNECTION` (or `SESSION_TARGET_CHANGED`) with the hint
  "attach the SAP session again (gui_session_attach) and retry". Found live: after a tray restart an old connection
  file was re-attached to a session on the logon screen (`A4H/000/`) instead of the previous `A4H/001/DEVELOPER`.

## 2026.10.25.2 (2026-10-25)

### Cloud link and stdio MCP: a closed default connection is re-attached

- After the link tray or SAP Logon restarted, the first tool call no longer fails with `INVALID_CONNECTION:
  Connection N session no longer exists` when exactly one SAP GUI session is open: fairyfly re-attaches the saved
  default connection to that session, runs the call and says so in the result ("re-attached to the only open SAP
  session"). Over the link this happens inside the approved call, so a change is approved once, not twice.
- The same holds for a default set by an earlier `gui_session_attach` (until now `SESSION_TARGET_CHANGED` after SAP
  Logon restarted). It applies only when that session is gone; a default that now names another live session is
  still refused with `SESSION_TARGET_CHANGED`.
- Only for the implicit default connection, and only when the open session is the same SAP system, client and user
  as the closed one. A saved connection now records them (`system`, `client`, `user` in `fairyfly.N.con`); for an
  older file without them the only open session is used and a warning is logged.
- Unchanged: an explicit `connection`, no or several open sessions, another system/client/user, and the HTTP endpoint
  still get `INVALID_CONNECTION` (or `SESSION_TARGET_CHANGED`).

## 2026.10.25.1 (2026-10-25)

### Cloud link: "Allow changes in SAP" is the approval (approval model v2)

- **Allow changes in SAP** in the link tray no longer switches between read-only and write mode. It is the standing
  approval at this desktop: on, the cloud's changes run without asking; off (the default), every change asks with the
  notification and a yes approves that one change. Changes are possible either way; they still need the organisation's
  grant.
- The switch is saved for the Windows user and stays as it is after a restart or an update.
- The approval window's **Don't ask again** (was: for 5 minutes) approves the change and turns on **Allow changes in
  SAP**.
- When the organisation requires approval (fairyfly.io settings: "Always ask at the desktop", gateway v0.5.8), the
  switch and the checkbox are greyed out ("Approval is required by your administrator") and every change asks. The
  saved switch comes back when the organisation lets the user decide again.
- libffl synced from the gateway (v0.5.8): every cloud change asks this desktop, whatever the gateway's grant says
  about approval; the audit tells `standing` (ran under **Allow changes in SAP**) from `approved`.
- No notification for a change fairyfly refuses anyway (`--read-only`, `FAIRYFLY_READ_ONLY`).
- Keys are named in the approval: "press Save (Ctrl+S)" instead of "press 11".
- `link run --allow-write` is still accepted and has no effect.
- A cloud call gets write rights only for the change approved at this desktop (or by **Allow changes in SAP**),
  whatever the gateway's grant says about approval; any other call runs read-only.

### MCP: navigation is annotated read-only

- `gui_transaction_start`, `gui_session_attach` and `gui_element_f4` now carry `readOnlyHint: true` (with
  `destructiveHint: false`, as before). They change no SAP data: they open a transaction's start screen, attach to an
  open session (saving a local connection file) and open the F4 value help. Gemini Enterprise asks the user to
  confirm every tool without `readOnlyHint: true`, so it no longer asks before each navigation step; after updating,
  reload the connector's custom actions in Gemini Enterprise. Annotations are hints for clients only: the read-only
  guard, token scopes, T-code allowlists and the cloud link's desktop approval are unchanged. `gui_connection_list`
  (cleanup deletes saved connection files) and `gui_popup_close` (vkey 0 confirms) stay not read-only.

### Cloud link: approval follow-ups (fairyfly #36)

- "Don't ask again for 5 minutes" covers exactly the assistant it was given for. It was keyed by the assistant's
  name cut to 24 characters, so a yes for Claude Code (`https://claude.ai/oauth/claude-code-…`) also approved every
  other client whose id starts with `https://claude.ai/oauth/` (same account, 5 minutes). It now uses the whole client
  id (libffl `ApprovalPrompt::client_id`), and the approval window shows all of it.
- A click on Approve in the window that arrives after the 45 s are up, or after the screen locked, is no longer
  taken as a yes (and remembers nothing): the window notices the deadline only every 200 ms.
- The same for the notification: when Deny (or Approve) in the notification comes before a click in the open approval
  window, the notification's answer counts. A Deny there is no longer overruled by Approve with "Don't ask again" in
  the window, and the window's tick remembers nothing.
- Closing a popup with F12 (Cancel, the default) or Shift+F3 no longer asks for approval. `gui_popup_close` stays
  annotated as no change; over the cloud link only a key that confirms the popup (vkey 0, Enter) asks.

## 2026.10.25 (2026-10-25)

### Cloud link: the gateway learns why a change was refused

- The link reports each approval's outcome to the gateway (libffl from fairyfly-gateway #121, approval/1): approved,
  remembered ("Don't ask again"), denied, expired or locked. The assistant gets APPROVAL_DENIED, APPROVAL_EXPIRED or
  APPROVAL_LOCKED instead of one generic refusal, and the gateway's audit records the outcome.
- An unanswered approval expires after 45 s (was 90 s), so the answer reaches the assistant before its MCP client
  gives up.

## 2026.10.24.2 (2026-10-24)

### Cloud link: changes are approved at this desktop

- There is no phone approval any more (fairyfly-gateway #115): a change an AI assistant makes through the cloud link
  is approved by the person at the desktop where SAP GUI runs. The link shows a Windows notification
  "<assistant> wants to change SAP: VA02 - fill 3 fields, then save" with **Approve**, **Deny** and **Details**; it
  stays on screen until answered and can be answered from the notification centre too.
- **Details** (or a click on the notification) opens the approval window: the assistant by name ("Claude", not
  "https://claude.ai/oauth/", which follows in brackets), the account it acts for, the SAP system, client and
  transaction, every step numbered (the standard toolbar's buttons named, "click Save (...btn[11])") and a countdown.
  **Deny** is the default button and Esc.
- Every change asks; a yes approves that change only. There is no blanket "allow SAP changes for the next 5 minutes"
  any more: only the window's **Don't ask again for 5 minutes**, unticked by default, approves further changes of the
  same assistant for the same account without asking (never while the screen is locked).
- No answer in 90 s, a locked screen, a disconnected or non-interactive session: denied. Several questions are asked
  one after another, never together. Each decision is in the log.
- The old Yes/No box is gone, and with it its bug: after its 60 s it stayed open although the change was already
  denied. With notifications off for fairyfly the approval window opens at once.
- The Start-menu entry carries fairyfly's AppUserModelID (`DataZoo.fairyfly`) and the notifications' COM activator;
  an uninstall removes their registration.
- The tray's and `link run`'s texts say "at this desktop" instead of "or the phone".

### Security

- `link pair` checked the gateway address by string prefix, so `http://localhost.evil.example`,
  `http://127.0.0.1@evil.example`, `http://localhost:80@evil.example` or `http://127.0.0.1.nip.io` sent pairing
  traffic to another host without TLS. The address is now parsed (with the same strict parser the HTTP and WebSocket
  clients use): `https://` is required, plain `http://` only for exactly `localhost`, `127.0.0.1` or `[::1]` (optional
  port, no userinfo). This holds for `--gateway`, `FAIRYFLY_LINK_GATEWAY` and the typed address; the link address the
  gateway hands out at pairing must be `wss://` (`ws://` only to those hosts), and `link run` / `link status` refuse a
  stored pairing with another one (`LINK_STATE_INSECURE`).
- One link per pairing now holds for every spelling of the pairing file: the lock name comes from the file's final
  path (junctions, symbolic links, subst drives and 8.3 short names resolved, Unicode case folded), not from the path
  text, so a second `link run` through a junction, a short name or a non-ASCII case difference is refused too.

### Releases

- The MSI version is now `(YYYY-2000).MM.(DD*100+N)`: `2026.10.25` is `26.10.2500`, `2026.10.25.1` is `26.10.2501`.
  Windows Installer ignored the 4th field of the old `26.10.24.1`, so a `26.10.24` MSI silently replaced an installed
  `26.10.24.1`; a same-day downgrade is now refused ("A newer version of fairyfly is already installed."). The
  released `26.10.23`, `26.10.24` and `26.10.24.1` MSIs upgrade to the new versions as before, and a rebuilt MSI of
  the installed version still replaces it. Add/Remove Programs and the winget `DisplayVersion` show the new number.
  A same-day release number is now 1 to 99 (`set_version.ps1`, `msi_version.ps1`, `New-WingetManifest.ps1`).
- The release workflow's tag check is case-sensitive (`packaging/check_release_tag.ps1`, covered by
  `packaging/tests/Test-VersionScripts.ps1`): a tag such as `V2026.10.24.1`, which the updater never takes, is
  refused, as it is by `msi_version.ps1` and `New-WingetManifest.ps1`. It and the configure check also limit the same-day
  release number to 1..99 like the MSI version (the updater still reads tags up to `.65535`).
- Configuring checks that the version is a real date (`cmake/calendar_version.cmake`, with the updater's leap-year
  rules): a hand-edited `2026.2.30` or `2026.02.30` fails with a clear message. Covered by `Test-VersionScripts.ps1`.

### Cloud link

- The link tray no longer stops with "The cloud link stopped (exit code 2)" when a console `link run` for the same
  pairing starts just before the tray's own link: it shows its notice, waits and connects once the console link
  ends. `link run` refused with `LINK_ALREADY_RUNNING` now exits with code 3 (other refusals stay 2).

### Trays: starting like a Windows app

- The Start-menu entry **fairyfly** no longer flashes a console window: it starts `fairyfly-tray.exe`, a small
  Windows program installed next to `fairyfly.exe`, which starts the tray without a console (`fairyfly.exe` is a
  console program; Explorer opened a console for it before the tray hid itself). The Run values for "Start with
  Windows" use it too.
- The link tray's menu has **Start with Windows** (a check mark). It is turned on once, at the first start from the
  Start menu or the installer; after that only the menu changes it. A Run value of an older version that started
  `fairyfly.exe` directly is moved to `fairyfly-tray.exe`, its options kept.
- Starting a tray that already runs (Start menu again) opens its menu at the icon instead of the "already running"
  message box.
- Either mouse button opens a tray's menu; its first line, in bold, is the status (left-click showed a balloon).
- Icons: the cloud link tray is teal, the local MCP server's tray violet, so both can be told apart side by side.
  States are a badge (amber: warnings or paused, red: error) or a faded mark (stopped), no longer a recoloured mark.
- `fairyfly.msi`: asks running trays to quit before it replaces or removes the files (no reboot because of a running
  tray), starts the tray after an interactive install (`LAUNCHAPP=0` turns that off; never with `/qn`), and an
  uninstall removes the trays' Run values of its folder and their settings.

## 2026.10.24.1 (2026-10-24)

### Releases

- A second release on the same day is possible: version `YYYY.MM.DD.N` (N = 1, 2, ...), tag `vYYYY.MM.DD.N`, e.g.
  `2026.10.24.1`. `fairyfly update check|apply` orders `2026.10.24 < 2026.10.24.1 < 2026.10.25` (three-part tags keep
  working; malformed ones such as `v2026.10.24.0` or `v2026.10.24.01` are refused). The exe's version information
  carries N as its 4th field, the MSI version is `26.10.24.1` and replaces an installed `26.10.24` (and is replaced by
  the next day's MSI), the winget package version is `2026.10.24.1`. `packaging/set_version.ps1` sets the version in
  `version.h`, `CMakeLists.txt` and `vcpkg.json`; configuring fails when they differ.

### Cloud link: pairing and one link per pairing

- `link pair` without `--gateway` and `FAIRYFLY_LINK_GATEWAY` asks for the service with `https://fairyfly.io`
  filled in: Enter accepts it, or edit it (in a console the address stands in the input line; from a pipe it is
  shown in brackets and an empty answer takes it). `--gateway` and `FAIRYFLY_LINK_GATEWAY` still decide without a
  question.
- `link run` (also the link tray's link) holds a per-pairing lock while it serves; a second link for the same pairing
  state is refused with `LINK_ALREADY_RUNNING` instead of fighting the first over the connection. The link tray
  waits (with a notice) while a console link serves its pairing and connects once that one ends.
- `link pair`'s `next` no longer says `fairyfly link run` when the link tray is running (it connects by itself) or a
  link for this pairing already runs (restart it to use the new pairing).

## 2026.10.24 (2026-10-24)

### Fill a whole screen in one call

- `gui_element_fill` takes `fields` (up to 50 `{element, value}` entries of one screen, grid cells and checkboxes
  too) and an optional `then_key` (`enter` or `f8`), CLI `fairyfly element fill --fields <json|@file> [--then-key
  enter|f8] [--keep-going]`. Every field runs through the same checks as a single fill (read-only, selection-input
  and `--tcode` token rules per field); the key is pressed only on the main window and only when every field
  succeeded. Save, Back and Cancel stay separate calls. The audit trail records `--fields` as `<redacted>`.
- `gui_screen_read` with `only=editable` lists empty input fields with their ids (no longer compact by default), so a
  blank entry screen can be filled. The tool descriptions, `gui_batch` and the server instructions point the model to
  the one-call fill; a second single-field fill within two minutes returns a hint to use `fields`.
- Cloud link (libffl from gateway v0.5.5): every step of a `gui_batch` is checked on the desktop too (granted, not
  dangerous, not nested, transactions per step); a batch or screen fill that writes needs write mode and one approval.

### Update

- `fairyfly update` failed for every release download with "Failed to read connection": github.com's redirect to a
  release asset now carries a ~9 KB `Content-Security-Policy` header, above cpp-httplib's 8 KiB limit. The limit is
  raised to 64 KiB. A dropped connection on a download host is now retried like an HTTP error.

### Start menu and trays

- The Start-menu entry "fairyfly (tray)" did nothing: it ran `mcp --tray`, which refuses to start without the HTTP
  transport (`TRAY_REQUIRES_HTTP`) and printed that to a console that closed at once (found on a Windows 10 VM with
  2026.10.23). The entry is now **fairyfly** and starts the cloud link tray (`fairyfly link run --tray`). An MSI or
  winget upgrade removes the old entry; `fairyfly update apply` only replaces the exe, so after it the old entry stays
  (it now shows the `TRAY_REQUIRES_HTTP` message): install the MSI once, or delete the entry. Pins of the old entry
  on the taskbar or Start have to be pinned again.
- The link tray no longer refuses an unpaired desktop (`LINK_NOT_PAIRED`). It starts with a grey icon and a notice,
  offers **Pair with the fairyfly cloud...** (`link pair` in a console) and starts the link as soon as the pairing is
  saved.
- The link tray's menu has **Start local MCP server**: it starts the MCP server in its own tray
  (`fairyfly mcp --tray --http`). While that tray runs, the item is checked and disabled (single-instance guard
  `Local\fairyfly-tray`).
- Startup errors of `mcp --tray` and `link run --tray` (`TRAY_REQUIRES_HTTP`, an invalid config file, a failed relaunch,
  a tray window that cannot be created) are shown in a message box when no shell console would show them (Start
  menu, autostart, started from the link tray).
- When the taskbar is not there yet (an autostart right after logon) a tray keeps running and adds its icon later,
  instead of ending; notices wait for the icon, and after 60 s without one a message box says so. A second start
  from the Start menu says "already running" in a message box. Consoles opened from a tray no longer inherit
  `FAIRYFLY_TRAY_CHILD`.
- `fairyfly.msi`: `AllowSameVersionUpgrades`, so a rebuilt MSI of the same version replaces the installed one instead
  of being installed beside it.
- CI (`winget.yml`): after the unelevated install it starts the Start-menu shortcut the way Explorer does (new console,
  its working directory, an isolated `LOCALAPPDATA`) and requires the tray to be running 10 s later. It also upgrades
  the 2026.10.23 MSI and checks that "fairyfly (tray)" is gone.

### Cloud link: audit trail with screenshots

- `fairyfly link run` supports the gateway's per-desktop audit trail (docs/LINK.md, "Audit trail"; announces
  `trail/1` and `trail-blur/1`). While an admin has it on, every step an AI assistant takes on this desktop is
  recorded with a short description, screenshots of the SAP window before and after it, and the field changes
  (old → new) of steps that change SAP.
- Password fields are always, bank fields (IBAN, account number, bank key, SWIFT/BIC, card numbers, account holder)
  by default filled solid on the screenshots before they leave the desktop, and their values masked (`••••`) in the
  field changes. A screenshot whose sensitive field cannot be located is not sent.
- The person at the desktop sees it: a console line in `link run`, an entry and a balloon in the link tray and the
  `mcp --tray` Cloud link submenu, and `audit_trail` in `fairyfly link status`.
- `link pair` and `link run` print the state file they use first; `FAIRYFLY_LINK_FORBID_DEFAULT_STATE=1` makes every
  `link` verb refuse the default state in `%LOCALAPPDATA%\fairyfly\link\` (tests and test installations).
- `tests/integration/link_trail_live.ps1`: live check on a separate test pairing (blurring of the current SAP
  screen, notice, status, recorded steps through a gateway).

## 2026.10.23 (2026-10-23)

### Cloud link (`fairyfly link`)

- New noun `link` (`pair`, `status`, `run`, `pause`, `resume`, `unpair`; docs/LINK.md):
  - It connects this desktop to the fairyfly cloud gateway, outbound only, so AI assistants connected there can use
    the SAP GUI.
  - `link run` serves the calls through fairyfly's own MCP dispatcher in-process, with the same read-only, family
    and policy guards as `mcp`, plus the gateway's per-call grant.
  - Changes need the person's approval on the desktop or on the phone.
  - Calls are audited with `audit_source: "link"`.
- Pairing shows a QR code and a number; nothing is saved before the person at the desktop confirms the account and
  organisation shown.
- `link pause` / `link resume`: a paused link is disconnected, so the AI assistant cannot reach the desktop until it is
  resumed. The running link follows a marker file next to the pairing state and writes its status beside it.
- Tray (`mcp --tray`): a **Cloud link** submenu with the pairing, the link state, Connect/Pair again, Start link,
  Pause/Resume and a health check.
- `tests/integration/link_live.ps1`: a live check through a real gateway (read call, audit, pause, resume).
- What the console shows:
  - The pairing QR code reaches a Windows console intact whatever its code page (`WriteConsoleW`; it used to come
    out as mojibake in cp437/1252 consoles). Where block characters cannot be shown it falls back to ASCII;
    `--qr-style auto|blocks|ascii` overrides that.
  - `link pair` prints `Contacting <gateway>…` at once.
  - `link run` prints its connection state (connected, lost and reconnecting, paused, resumed, stopped).
  - Ctrl+C stops both within a second.
  - `link pair|status|pause|resume|unpair` no longer attach to SAP GUI before they start.
- `fairyfly update check|apply`: updates from the project's GitHub releases, verified against `SHA256SUMS` and the
  Authenticode signer (a signed installation only accepts the same signer) before `fairyfly.exe` is replaced (the old
  one stays as `fairyfly.exe.old`). The tray checks once in the background (`FAIRYFLY_UPDATE_CHECK=0` turns it off)
  and has a "Check for updates" item.
- `fairyfly.msi`: a per-user installer (`packaging/fairyfly.wxs`, WiX 5): `%LOCALAPPDATA%\Programs\fairyfly`, the
  user's `PATH` and a Start-menu entry for the tray. The CI installs and uninstalls it; releases carry it, signed when
  signing is configured, and list it in `SHA256SUMS`.
- winget: manifests for `DataZoo.fairyfly` (schema 1.10.0, `Scope: user`, `ElevationRequirement:
  elevationProhibited`) rendered per release by `packaging/winget/New-WingetManifest.ps1` from the MSI (SHA-256 and
  ProductCode through the Windows Installer API) and attached to each release. CI (`winget.yml`) validates them and
  installs and uninstalls fairyfly with `winget` from an unelevated process on Windows Server 2022.
- `third_party/libffl`: the link protocol library, vendored from the gateway (`scripts/sync_libffl.py`). It adds
  OpenSSL to the vcpkg dependencies.

## 2026.10.05 (2026-10-05)

First published release. It also contains everything listed under 2026.09.30 below, which was prepared but never tagged.

### Parallel sessions

- Owner-mode HTTP `gui_screen_read`: `structuredContent` held only `screen_guard`, and Claude Code, which hands `structuredContent` to the model when present, saw every screen read as empty (found while rehearsing the multi-agent README demo). A Markdown result now carries the screen text in `structuredContent.text` next to `screen_guard`.
- Owner-mode endpoints no longer fall back to an embedded SAP GUI when SAP Logon is not running. The fallback (`SapGui.ScriptingCtrl` created inside the tray) opened windows that the session workers, which attach through the running object table, could never reach: agents logged on, then every call failed with `OWNER_IDENTITY_UNKNOWN`. The tray and its workers now switch the fallback off, and `gui_session_launch` answers `SAP_LOGON_NOT_RUNNING` with a hint (passed through the owner-mode refusal, because it reveals nothing about other sessions). If SAP Logon runs but its scripting engine does not answer, the error says so instead. The CLI and the stdio server keep the fallback.
- `tests/integration/demo/`: scripts and runbook for the README demo. `prepare.ps1` creates one read-only token per agent and starts the tray (owner mode, SAP Logon started if needed); `launch_agents.ps1` starts one Claude Code agent per scenario in a themed Windows Terminal, each agent opens and logs on to its own SAP GUI window, and the script places every window above its agent by following the MCP audit trail; `cleanup.ps1` undoes it. Recording with VirtualBox's own recording function is described in the runbook.

### Fixes from the Codex live bug hunts (2026-10-03)

- `--read-only` no longer lets a click on the Yes/OK button of a popup through (found live: Yes on the SU01 Delete Users dialog deleted the user); popup buttons are allowed only when they dismiss or navigate, also in the `@active/...` id form. Clicking a check box or radio button of a selection screen stays allowed under `--read-only` (documented decision).
- `element click` on a check box toggles it (it only ever checked it); `element fill` accepts true/false/1/0/yes/no/on/off/x for check boxes and radio buttons and the key or the displayed text for combo boxes (`INVALID_ARGUMENT` lists the available entries); the fill/click result reports `selected`, `key` and `display_value`.
- Grid paging: `screen read --offset N` on a lazily loading ALV grid (SM21, 2,732 rows) returned empty pages until something scrolled the grid there; the reader now scrolls to the rows a window needs and restores the scroll position. The first far scroll can cost SAP up to a minute once.
- Classic lists: the header row is the row the data rows align with, not the busiest row, so the SE16 result for TVARVC is a table again.
- `screen capture --format base64 --file` writes the file; a date-format hint taken from the field's previous text no longer produces a warning (it may be an earlier rejected input); error and hint polish (`TAB_NOT_FOUND` carries `tab_id` and `reason`, the inactive-tab hint names the tab id, `element get` answers `ELEMENT_NOT_FOUND`).
- USR02 credential-hash columns OCOD1-OCOD5, PWDSALT and PWDHISTORY (also table-qualified names such as `USR02-OCOD1`) are redacted like BCODE.

### Bulk screen reader (default)

- `screen read` can read the whole screen tree with one `GuiSession.GetObjectTree` call: `FAIRYFLY_SCREEN_READER=auto|bulk|legacy` (hidden `--tree-reader` overrides per read). **Bulk is the default** (`auto`): the output shape does not change (no `diagnostics` block unless a mode is chosen explicitly or the read had to fall back) and `FAIRYFLY_SCREEN_READER=legacy` restores the per-element reader. The output (`data.*`) is identical to the per-element reader on 18 live screens and 7 recorded fixtures; on Bigfox the 12 screen reads of the 10-task workload took 8.4 s legacy and 5.4 s bulk (-35%, whole workload -15%). Only 12 vetted properties are requested; `Selected` is never requested because it makes SAP GUI raise `RPC_E_SERVERFAULT` (and probably crashed SAP Logon), `AccLabel` and `Selected` are read per element. Password fields are blanked right after parsing, the raw tree is never logged. `auto` falls back to the per-element reader on any problem and reports why in `diagnostics.screen_reader`; `bulk` returns `OBJECT_TREE_UNAVAILABLE`; a server fault or three invalid answers in a row switch the bulk path off for the process. `--probe-all`, `screen find` and structure-less reads stay on the per-element reader.
- Diagnostics (hidden): `screen read --dump-object-tree` prints the raw, unredacted tree and `FAIRYFLY_BULK_FAULT=unsupported|garbage|wrongroot|fault|exception` injects tree failures; both need `FAIRYFLY_DIAG=1`.
- Shared, tested implementations of the display-text/redaction decisions and the plain-element metadata (`display_text_policy`, `element_metadata_builder`) now serve both readers.

### Performance (speed round 3)

- `transaction start` no longer sleeps a blind 100 ms after `SendCommand`/`StartTransaction`; `wait_for_completion` checks `Busy` immediately and polls every 20 ms (`SESSION_POLL_INTERVAL_MS`). The unused `ComGuiSession::send_vkey` was removed.
- HTML viewer reads (`read_html_viewer_text`) have a 300 ms budget (50 ms poll), retry only while the UI Automation document is missing or empty (a Codex review pointed out that a browser window may not have created its document yet), and reuse one `IUIAutomation`. `content_available` is unchanged on SM21 and RZ11 (it was already false there); a window-class gate for the generic shell probe exists but is off (`kGateGenericHtmlProbe`).
- DISPID lookup: a validated process-wide `Type` DISPID and type-first resolution for fresh wrappers; `Changeable` is read once per wrapper and `DisplayedText` misses on GuiShell are cached. Status bar reads need 5 round trips (3 with a known window id; message id/number only when a message exists). Dead response serialisation in `run_cli` removed.
- `tests/integration/compare_builds.ps1 -FullCompare` deep-compares elements, hierarchy, tabs and status bar between two builds; `-Screens` no longer clobbers the session id.
- Measured on Bigfox (10 Basis tasks, 45 invocations, best of 3, debug logging off): 26.5 s to 21.9 s (-17%); screen content identical on 18 screens.

## 2026.09.30 (2026-10-02)

### Added (release engineering)

- `LICENSE`: Business Source License 1.1 (licensor DataZoo GmbH, same terms as DataZooDE/erpl), `THIRD_PARTY_NOTICES.md`, README sections "Download", "License" and "Code signing" (including a privacy statement).
- Version information resource in `fairyfly.exe` (`ProductName fairyfly`, `ProductVersion` equal to `fairyfly --version`), checked in CI.
- `release.yml`: tag-triggered build and test, optional Azure Artifact Signing, GitHub release with `SHA256SUMS`; `docs/SIGNING.md` (the free SignPath Foundation programme is not available for a non-OSI licence); `.github/CODEOWNERS`.

### Changed

- Remote MCP, stateless era aligned with the published 2026-07-28 specification (verified 2026-10-02): a request is modern when `_meta` (prefixed `io.modelcontextprotocol/protocolVersion` or plain) says 2026-07-28 or later, the method is `server/discover` or the `MCP-Protocol-Version` header is 2026-07-28. Modern requests must carry `MCP-Protocol-Version` (equal to the body), `Mcp-Method` and, for `tools/call`/`resources/read`/`prompts/get`, `Mcp-Name` (with `=?base64?...?=` decoding); otherwise HTTP 400 with JSON-RPC -32020 naming the header, before any tool runs. Modern unknown methods, `ping` and `logging/setLevel` answer 404 with -32601; modern results carry `_meta["io.modelcontextprotocol/serverInfo"]`; prefixed and plain `_meta` keys that disagree are 400 -32602. Legacy (2025-06-18, 2025-11-25) requests are unchanged. Clients that sent 2026-07-28 requests without these headers now get 400.

### Added

- Verb-level token scopes for the remote MCP endpoint: next to the family scope (`session`) a token can carry `<family>.<verb>` (`session.list`, `session.attach`, `screen.read`, `element.get`, ... one tool each; the valid verbs are generated from the command table, `gui_doctor` and `gui_batch` stay family-only). A call is allowed with the tool's family, its exact verb scope or `*`; `tools/list` and `gui_batch` items follow the same rule, `SCOPE_DENIED` now reads `token 'x' lacks scope 'session.disconnect' (or 'session')`. New error code `UNKNOWN_SCOPE` (unknown verb of a known family). The default scope of `mcp token create` is now `session.list,session.attach,connection.list,screen` (read-only), so a default token can no longer launch, log in or disconnect; existing tokens keep their family scopes unchanged. `--scope` input is case-insensitive.
- Remote MCP token option `--allow-selection-input` (needs `--tcode` and a read-only token; shown by `token list`, kept by `rotate`): the token may type into plain selection fields with `gui_element_fill`, but only on the initial screen of the transaction it started with `gui_transaction_start` (program and screen number recorded per token), never into grid cells, the command field or password/credential fields, and never commit (the read-only guard stays for every other action). New refusal codes `INPUT_NOT_ALLOWED`, `INPUT_SCREEN_DENIED`, `INPUT_TARGET_DENIED`; audit records of allowed fills carry `input_allowed: true` (never the value). SAP facts now also read `Info.Program` and `Info.ScreenNumber`.

- Remote MCP listener on http.sys (the Windows HTTP Server API) with in-kernel TLS: `fairyfly mcp --http --tls` serves HTTPS on the certificate bound by setup; the server runs unelevated and holds no private key.
- `fairyfly mcp setup` (certificate, URL ACL, TLS binding, optional firewall rule; self-elevates once; `--dry-run`, `--print-runbook`, `--yes`, verified by a real TLS round trip), `fairyfly mcp teardown` and `fairyfly mcp cert export`; `fairyfly mcp doctor` extended with elevation, URL ACL, TLS binding, certificate, firewall, port and TLS handshake checks.
- Config keys `server.tls` and `server.allow_ip`; flags `--tls/--no-tls`, `--allow-ip` and the flag-only `--insecure-http`.
- Server-level IP allow-list (`--allow-ip`, addresses or CIDR blocks, loopback always allowed): other peers get 403 `ADDRESS_NOT_ALLOWED` before anything else is checked.
- `mcp client-config` prints https URLs and the certificate trust hints (curl `--cacert`, `NODE_EXTRA_CA_CERTS`, `certutil`).

### Screen read

- `--text-contains` across tabs reports `tabs_searched`, `tabs_skipped` (`not_expanded`, `not_requested`, `busy_timeout`, `not_found`, `error`) and a `text_filter_note` (`0 matches in tabs [A]; tabs not expanded: [B, C] (use tab=...)`, also a Markdown line) when a tab was not searched or nothing matched, instead of a silent empty result. `--tab` still reads only that tab.
- `element fill` result: `value` is the value read back from the control (max 200 characters; credential fields keep `[REDACTED: reason]` and `value_redacted: true`), new `field` object (`type`, `max_length`, `numerical`, `required`, `input_kind`, `format_hint`, `format_hint_source`, `value_normalized`, `format_warning`; never a rejection) and `status_bar` / `status_message` only when the bar changed during the fill (a stale message from the previous action is no longer echoed).
- New error code `ELEMENT_ON_INACTIVE_TAB` (`element get`, `element click`, `element fill`; fields `tab_id`, `tab_text`, `tab_strip_id`, `hint`) and `element get --activate-tab` (MCP `activate_tab`), which selects the tab, reads the element and restores the previous tab (`tabs_activated`, `tabs_restored`).
- `element get` and `screen find` (JSON, Markdown, TOON) add `tooltip` for GuiButton, GuiTab, GuiCheckBox and GuiRadioButton, and `text` in `element get` when it differs from `value`; absent when empty.
- `screen capture`: the crop is always in native window pixels and applied before `--scale` (stated in the CLI help and the MCP tool text); the result reports `native_size`, `crop`, `crop_clamped` and `output_size`; a crop completely outside the window is `INVALID_ARGUMENT` with the native size; every crop goes through the processing path.
- `session list` and `session attach` report `server_time: null` / `server_time_source: "unavailable"` (per session in the list), `server_time_summary`, `server_time_note`, `client_time` and `client_utc_offset`: the scripting API exposes no SAP server clock, the PC clock is given as a labelled stand-in.
- `screen read --offset N` / MCP `offset`: grid and table reads can start at any row; the table JSON gets `offset`, `returned`, `total`, `next_offset` and `exposed_rows`, trailing empty padding rows are trimmed (`empty_rows_trimmed`) and the Markdown header explains `Total Rows`, `Visible Rows (viewport)` and `Returned Rows`.
- `--text-contains` filters the rows of grids and table controls (`rows_matched` / `rows_total`).
- The `only` selectors report the grids and trees they dropped (`suppressed`, plus a Markdown note); new `--only-tables` / `only=tables`.
- Redaction markers say why (`[REDACTED: field name matches password pattern]`); credential state flags such as `PASSWORD_EXT_PWD_STATE`, role names and profile names are no longer hidden for the words they contain.
- Fewer COM round trips: grid cells are not read when an `only` selector discards all grids, table-control columns and `--tab` strip lookups use one enumeration instead of an index loop.

### Changed

- cpp-httplib is removed; plain HTTP remains available on loopback for development and tests.
- The trusted-proxy mechanism is removed: `X-Forwarded-*` and `X-Fairyfly-Proxy-Secret` are ignored, the client address is the socket peer address (token `--ip` and audit `remote_addr` use it), the "proxy secret not set" warning and the `auth.proxy_secret_source` key are gone. A leftover Credential Manager entry `fairyfly:fairyfly-mcp-proxy` is unused and can be deleted.
- `server.host` is the URL prefix host: `127.0.0.1` (default), `+` (all interfaces) or a host name; `localhost` is treated as `127.0.0.1`. Plain HTTP on a non-loopback host is refused unless `--insecure-http` is given.

### Security

- Action status: a fresh type-W status message that asks for confirmation (EN/DE: "Press ENTER to continue", "Confirm ...", "Bestätigen Sie ...", "weiter mit Enter") after a submitting action is no longer reported as success; the result is an `ACTION_OUTCOME_UNVERIFIED` error with the message, `needs_confirmation: true` and a hint to send Enter if intended. Other W messages stay success with `warning: true`.

- Redaction: the "credential state name" exemption (`PASSWORD_EXT_PWD_STATE`, `Password status`, ...) now applies to input fields only when the field is known to be display-only, is not a password field and its value looks like a state label; a changeable or unknown-changeability field with such a name (and every fill echo) stays redacted. Display-only state fields (SU01 display) are still shown; labels, grid and report cells keep the exemption.

- Remote MCP over HTTP: `Mcp-Method` / `Mcp-Name` headers that are present are now validated against the body for legacy (2025-xx) requests too (400 + -32020, no provider call), and an `MCP-Protocol-Version` header naming a served version that differs from the body `_meta` protocol version is rejected in every era. Absent headers stay accepted on legacy requests.

- Documentation and tool texts: the `gui_element_fill` descriptions no longer claim that values are echoed nowhere (the result echoes the value read back, except for credential fields); `mcp token create --help`, docs/MCP.md and docs/MCP_REMOTE.md state that Enter/F8 EXECUTE a selection, so a token with `--allow-selection-input` must only list transactions whose execution is read-only.
- `element get --activate-tab` / `activate_tab`: the previously selected tab is recorded before each selection and restored through a scope guard on every exit path (read errors, wait failures, exceptions); a failing restore is reported as `tabs_restored: false` with `restore_error` (also on the error result of a failed read).
- Selection input: the guard lift of an authorized `gui_element_fill` now comes with handler-level live validation (`CommandHandler::set_selection_input_only`, restored on every path): the live control must be a changeable `GuiTextField`/`GuiCTextField` whose id, name, live label and tooltip name no credential (never password fields, combo boxes, check boxes, grid cells, ...), and the live program + screen number are re-read right before `SetText` and must equal the recorded initial screen, which also closes the window between the pre-call check and the write.
- Selection input (`--allow-selection-input`) hardening: the recorded initial screen is bound to the connection of the `gui_transaction_start` as well (a fill on another connection is `INPUT_SCREEN_DENIED`) and is cleared by the first later call of the token whose facts are empty or show another screen, so typing is only possible between the start and the first navigation (returning with F3 no longer re-enables it).
- Remote MCP T-code allowlist hardening: for a token with `--tcode`, `gui_menu_select` is denied and `gui_key_send` accepts only enter, f4, f8 and the page keys unless the token was created with the new `--allow-navigation` (only valid with `--tcode`, shown by `token list`). After every screen-acting call the transaction is read again; a token that ended outside its allowlist gets `tcode_left_allowlist` in the result (and the audit record) and its next screen call is denied until an allowed `gui_transaction_start` succeeds. Tokens without `--tcode` are unchanged.
- Codex review round 7: (1) selection input binds typing to the connection id and SAP session the `gui_transaction_start` actually used (taken from its result and the facts lookup, also under automatic single-connection resolution); an unknown or different connection/session on either side is `INPUT_SCREEN_DENIED`. (2) The live control check reads Type, Id, Name, Changeable, labels and tooltips strictly: a failed read refuses with `INPUT_TARGET_DENIED` naming the property instead of turning into an empty string; members a control type does not have (AccLabel, a left label, tooltips) stay empty and allowed. (3) Credential-state values are shown only when they are empty, a short number/date/time, or made of words from a fixed EN/DE state vocabulary (`Production Password`, `locked`, `gesperrt`); the length/whitespace heuristic is gone, so a 7-character secret in a `PASSWORD_*_STATE` display field stays redacted. (4) A present `MCP-Protocol-Version` header must equal the body `_meta` version in every era (400 -32020), also when the header version is unsupported; an unsupported header alone is still -32022. (5) A confirmation-like W status after a submitting action is `ACTION_OUTCOME_UNVERIFIED` (`needs_confirmation`) even when its text equals the previous message, and question-style prompts (EN/DE: "Do you want to", "Continue? (Y/N)", "Moechten Sie", "fortfahren", "trotzdem", ...) count as confirmation prompts.

## 0.2.0

Breaking release: a hard switch to a noun/verb CLI and `gui_<noun>_<verb>` MCP tool names, with no compatibility aliases. See [docs/CLI.md](docs/CLI.md#migrating-from-the-flat-commands) for the full old-to-new tables.

### Changed (breaking)

- CLI restructured into groups: `session` (list, attach, launch, login, disconnect), `connection list`, `screen` (read, find, capture), `menu` (list, select), `element` (get, click, fill, f4), `key send`, `popup close`, `transaction start`, `credentials` (unchanged), and the root verbs `doctor` and `batch`. The old flat commands (`list`, `attach`, `launch`, `login`, `disconnect`, `connections`, `get`, `click`, `fill`, `press_f4`, `send-key`, `close`, `tcode`, `screen menu`) are removed.
- `serve` is now `mcp`. `fairyfly mcp [options]` starts the stdio MCP server; `mcp` has an optional subcommand (`mcp tools [--markdown]` prints the tool table).
- MCP tools renamed from `sap_*` to `gui_<noun>_<verb>` (for example `sap_click` is `gui_element_click`, `sap_tcode` is `gui_transaction_start`). Old names are unknown tools.
- `fairyfly batch` lines use the new command paths; `mcp` inside `batch` returns `MCP_UNAVAILABLE` (was `SERVE_UNAVAILABLE`).
- Audit records: `cmd` is the CLI path (`element click`, `session attach`, ...); server lifecycle records use `cmd: "mcp"`. `redact_argv` recognises the `element fill` shape.
- Error hints and generated Markdown hints name the new commands.

### Added

- Command table (`src/command_table.*`) as the single source for CLI paths, root help sections, tool names, tool families and the audit command.
- Root `--help` grouped by section (SESSION, CONNECTION, SCREEN, MENU, ELEMENT, KEY, POPUP, TRANSACTION, CREDENTIALS, MCP SERVER, SYSTEM, GLOBAL FLAGS). Global options also work after the noun/verb (`fairyfly screen read --read-only`).
- `mcp --tools <families>` restricts the exposed tool families (unknown family: exit code 99, `UNKNOWN_FAMILY`).
- `mcp tools [--markdown]` prints the tool table; docs/MCP.md embeds it and a unit test keeps it in sync.
- `ToolSpec::family` and `ServeOptions::families` (additive changes to the MCP types contract).
- Unit tests for the command table, help completeness, global-option fallthrough, the `--tools` filter and the removed command names.

## 0.1.0

Initial release: SAP GUI automation CLI (attach, launch, login, tcode, click, fill, get, screen read/find/capture, batch, credentials, audit trail) and the stdio MCP server.
