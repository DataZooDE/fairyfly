# Changelog

## 0.2.0

Breaking release: a hard switch to a noun/verb CLI and `gui_<noun>_<verb>` MCP tool names, with no compatibility aliases. See [docs/MIGRATION_0.2.md](docs/MIGRATION_0.2.md) for the full old-to-new tables.

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
