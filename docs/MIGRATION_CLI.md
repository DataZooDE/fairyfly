# Migrating to the noun/verb CLI

The noun/verb CLI (first released as 0.2.0, before the switch to calendar versions) is a deliberate breaking change: the flat CLI commands became a noun/verb tree, the MCP tools were renamed after that tree, and `serve` became `mcp`. There are no aliases. Old command names fail with the normal CLI11 "not expected" error, and old `sap_*` tool names are unknown tools.

The mapping is defined once in `src/command_table.cpp`; `fairyfly mcp tools` prints the current tool table.

## CLI commands

Every leaf keeps its options and behavior; only its path changed. Global options (`--log-level`, `-v`, `--output`, `--verbose-errors`, `--read-only`, `--no-audit`, `--audit-required`, `--version`) work before and after the noun/verb.

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
| `screen menu --select PATH` | `menu select PATH` |
| `get` | `element get` |
| `click` | `element click` |
| `fill` | `element fill` |
| `press_f4` | `element f4` |
| `send-key KEY` | `key send KEY` |
| `close` | `popup close` |
| `tcode CODE` | `transaction start CODE` |
| `credentials list\|set\|delete\|import-env` | unchanged |
| `doctor`, `batch` | unchanged |
| `serve [options]` | `mcp [options]` (new: `--tools <families>`; `mcp tools [--markdown]` prints the tool table) |

`menu select` takes the menu path as a positional argument (it was the value of `--select`); `--window` and `--connection` are kept on both menu commands.

### Things that change with it

- `fairyfly batch` lines are argv without the program name, so they use the new paths: `["element","click","wnd[0]/usr/btn[1]"]`, `transaction start SM37`. `mcp` inside `batch` returns `MCP_UNAVAILABLE` (it was `SERVE_UNAVAILABLE`).
- The audit trail `cmd` field holds the space-joined CLI path: `element click`, `session attach`, `menu select`, `transaction start` (before: `click`, `attach`, `screen menu`, `tcode`). The MCP server lifecycle records use `cmd: "mcp"` (before: `serve`). Existing audit files keep their old values; queries over both need to match both.
- `redact_argv` recognises the `element fill` shape: the value (the positional after the element id) is still replaced by `<redacted>`.
- Hints inside error results name the new commands (for example `Run 'fairyfly session attach'`).
- Scripts that pipe fairyfly output are unaffected: JSON result fields did not change.
- MCP client configs must launch `fairyfly.exe mcp` instead of `serve` (see `docs/examples/`).

## MCP tools

Tool names are `gui_` plus the CLI path joined by `_`. The noun is the tool family used by `mcp --tools`.

| Old tool | New tool | Family |
|---|---|---|
| `sap_sessions` | `gui_session_list` | session |
| `sap_attach` | `gui_session_attach` | session |
| `sap_launch` | `gui_session_launch` | session |
| `sap_login` | `gui_session_login` | session |
| `sap_disconnect` | `gui_session_disconnect` | session |
| `sap_connections` | `gui_connection_list` | connection |
| `sap_screen_read` | `gui_screen_read` | screen |
| `sap_screen_find` | `gui_screen_find` | screen |
| `sap_capture` | `gui_screen_capture` | screen |
| `sap_menu_list` | `gui_menu_list` | menu |
| `sap_menu_select` | `gui_menu_select` | menu |
| `sap_get` | `gui_element_get` | element |
| `sap_click` | `gui_element_click` | element |
| `sap_fill` | `gui_element_fill` (write tool) | element |
| `sap_press_f4` | `gui_element_f4` | element |
| `sap_send_key` | `gui_key_send` | key |
| `sap_close_popup` | `gui_popup_close` | popup |
| `sap_tcode` | `gui_transaction_start` | transaction |
| `sap_credentials_list` | `gui_credentials_list` | credentials |
| `sap_doctor` | `gui_doctor` | system |
| `sap_batch` | `gui_batch` | batch |

Tool arguments did not change. Claude Code shows the tools as `mcp__fairyfly__gui_*`, so permission rules and allow lists that mention `mcp__fairyfly__sap_*` must be updated. Inside `gui_batch` items, `tool` values must use the new names.

### `mcp --tools`

`fairyfly mcp --tools session,screen,element` exposes only the tools of those families (comma or space separated; families: session, connection, screen, menu, element, key, popup, transaction, credentials, system, batch). The set is fixed for the process: other tools disappear from `tools/list` and `tools/call`. An unknown family exits with code 99 and a structured `UNKNOWN_FAMILY` error on stderr.
