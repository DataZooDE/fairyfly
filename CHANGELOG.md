# Changelog

## Unreleased

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

Breaking release: a hard switch to a noun/verb CLI and `gui_<noun>_<verb>` MCP tool names, with no compatibility aliases. See [docs/MIGRATION_CLI.md](docs/MIGRATION_CLI.md) for the full old-to-new tables.

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
