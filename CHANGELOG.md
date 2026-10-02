# Changelog

## 2026.09.30 (unreleased)

Versioning switched to calendar versions (YYYY.MM.DD); earlier 0.x numbers are retired.

### Added

- Verb-level token scopes for the remote MCP endpoint: next to the family scope (`session`) a token can carry `<family>.<verb>` (`session.list`, `session.attach`, `screen.read`, `element.get`, ... one tool each; the valid verbs are generated from the command table, `gui_doctor` and `gui_batch` stay family-only). A call is allowed with the tool's family, its exact verb scope or `*`; `tools/list` and `gui_batch` items follow the same rule, `SCOPE_DENIED` now reads `token 'x' lacks scope 'session.disconnect' (or 'session')`. New error code `UNKNOWN_SCOPE` (unknown verb of a known family). The default scope of `mcp token create` is now `session.list,session.attach,connection.list,screen` (read-only), so a default token can no longer launch, log in or disconnect; existing tokens keep their family scopes unchanged. `--scope` input is case-insensitive.
- Remote MCP token option `--allow-selection-input` (needs `--tcode` and a read-only token; shown by `token list`, kept by `rotate`): the token may type into plain selection fields with `gui_element_fill`, but only on the initial screen of the transaction it started with `gui_transaction_start` (program and screen number recorded per token), never into grid cells, the command field or password/credential fields, and never commit (the read-only guard stays for every other action). New refusal codes `INPUT_NOT_ALLOWED`, `INPUT_SCREEN_DENIED`, `INPUT_TARGET_DENIED`; audit records of allowed fills carry `input_allowed: true` (never the value). SAP facts now also read `Info.Program` and `Info.ScreenNumber`.

- Remote MCP listener on http.sys (the Windows HTTP Server API) with in-kernel TLS: `fairyfly mcp --http --tls` serves HTTPS on the certificate bound by setup; the server runs unelevated and holds no private key.
- `fairyfly mcp setup` (certificate, URL ACL, TLS binding, optional firewall rule; self-elevates once; `--dry-run`, `--print-runbook`, `--yes`, verified by a real TLS round trip), `fairyfly mcp teardown` and `fairyfly mcp cert export`; `fairyfly mcp doctor` extended with elevation, URL ACL, TLS binding, certificate, firewall, port and TLS handshake checks.
- Config keys `server.tls` and `server.allow_ip`; flags `--tls/--no-tls`, `--allow-ip` and the flag-only `--insecure-http`.
- Server-level IP allow-list (`--allow-ip`, addresses or CIDR blocks, loopback always allowed): other peers get 403 `ADDRESS_NOT_ALLOWED` before anything else is checked.
- `mcp client-config` prints https URLs and the certificate trust hints (curl `--cacert`, `NODE_EXTRA_CA_CERTS`, `certutil`).

### Screen read

- `element fill` result: `value` is the value read back from the control (max 200 characters; credential fields keep `[REDACTED: reason]` and `value_redacted: true`), new `field` object (`type`, `max_length`, `numerical`, `required`, `input_kind`, `format_hint`, `format_hint_source`, `value_normalized`, `format_warning`; never a rejection) and `status_bar` / `status_message` only when the bar changed during the fill (a stale message from the previous action is no longer echoed).
- New error code `ELEMENT_ON_INACTIVE_TAB` (`element get`, `element click`, `element fill`; fields `tab_id`, `tab_text`, `tab_strip_id`, `hint`) and `element get --activate-tab` (MCP `activate_tab`), which selects the tab, reads the element and restores the previous tab (`tabs_activated`, `tabs_restored`).
- `element get` and `screen find` (JSON, Markdown, TOON) add `tooltip` for GuiButton, GuiTab, GuiCheckBox and GuiRadioButton, and `text` in `element get` when it differs from `value`; absent when empty.
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

- Remote MCP T-code allowlist hardening: for a token with `--tcode`, `gui_menu_select` is denied and `gui_key_send` accepts only enter, f4, f8 and the page keys unless the token was created with the new `--allow-navigation` (only valid with `--tcode`, shown by `token list`). After every screen-acting call the transaction is read again; a token that ended outside its allowlist gets `tcode_left_allowlist` in the result (and the audit record) and its next screen call is denied until an allowed `gui_transaction_start` succeeds. Tokens without `--tcode` are unchanged.

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
