# MCP server design and module contract

`fairyfly mcp` is an MCP (Model Context Protocol) server over stdio: newline-delimited JSON-RPC 2.0,
one message per line, nothing but protocol messages on stdout (logging goes to stderr). Handshake
protocol versions: 2025-11-25, 2025-06-18, 2025-03-26, 2024-11-05. `server/discover` is answered
with -32601 (method not found).

The frozen shared contract is `src/include/mcp/types.h` (namespace `fairyfly::mcp`). Change it only
through the orchestrator; three workers code against it in parallel.

## Module map

| Module (header in src/include/mcp, source in src/mcp) | Phase | Role |
|---|---|---|
| types.h | 0 (frozen; additive: `ToolSpec::family`, `ServeOptions::families` in 0.2.0) | Transport, ToolProvider, ToolSpec, Policy, ServeOptions, error codes |
| json_rpc | 1 | `parse_message`, `make_result`, `make_error` |
| transport | 1 | `StdioTransport` (binary mode, thread-safe writes), `StringTransport` (tests) |
| server | 1 | `McpServer`: initialize/ping/tools/list/tools/call, cancel, queue, timeout |
| run_mcp | 0 wiring, 1 tweaks | Option validation, policy, audit hook, dispatcher, server |
| tool_catalog | 2 | `read_tool_specs()` (`gui_screen_read`, `gui_screen_find`, ...) |
| result_shaper | 2 | CLI `Result` -> MCP `ToolResult` (caps, untrusted-data header) |
| dispatcher | 2 | `CommandDispatcher`, `make_registry_invoker` |
| tool_catalog_write | 3 | `write_tool_specs()` (`gui_element_fill`; `write_tool = true`) |
| policy | 3 | `check_call`, `tool_visible`, `RateLimiter` |
| mcp_audit | 3 | `make_mcp_audit_hook` -> audit trail records |

Phase 1 also owns the `// PHASE 1: hook` in `src/cli_entry.cpp`: `mcp` is special-cased like
`batch` (copy `McpCommand::options()` before the registry is rebuilt, then call `run_mcp`).
Stubs carry a `// PHASE n:` comment naming the owner.

## Threading model

- The main thread owns COM and the `CommandHandler`; every tool call runs there, one at a time.
- A reader thread reads the transport, answers `ping` and handles `notifications/cancelled`
  immediately, and enqueues `tools/call` requests.
- Calls are serialized FIFO. The queue is capped at 16 (`ServerOptions::max_queue`); more gives
  `kServerBusy`. The per-call timeout is a soft 120 s (`call_timeout_ms`); a call cannot be
  interrupted inside COM, it is answered as timed out when it returns.
- `write_line` is thread-safe; both threads use it.

## Tools and dispatch

- Tool names are `gui_<noun>_<verb>` (for example `gui_screen_read`) and come from the command table
  (`src/command_table.cpp`, one entry per CLI leaf); the noun is the tool family (`ToolSpec::family`),
  `doctor` is family `system` and `batch` is family `batch`. `mcp --tools <families>` filters the
  catalog once at start; filtered tools vanish from both `tools/list` and `tools/call`. `mcp tools
  [--markdown]` prints the table (`tool_table_text`), which docs/MCP.md embeds.
- A tool maps to CLI argv through `ToolSpec::build_argv` (argv without the program name,
  `std::invalid_argument` on bad arguments). The `Invoker` runs that argv through the normal command
  registry (private `CLI::App`, `register_all_commands`, `setup_all_commands`, parse without
  `app.exit`, `execute_active_command`) on the shared handler in batch mode. No command is
  reimplemented for MCP.
- The audit `command` of a call is the CLI path from the command table (`element click`, `session attach`).

## Policy

- Read-only is the default. `--allow-write` exposes tools with `write_tool = true`; `--read-only`
  with `--allow-write` is INVALID_ARGUMENT.
- `FAIRYFLY_READ_ONLY=1` is a hard cap: it forces read-only and ignores `--allow-write`.
- Write tools are hidden from `tools/list` and refused at call time in read-only mode; the handler
  also gets `set_read_only(policy.read_only)`.
- Rate limit (default 120 calls/min), result size cap (60000 chars), image cap (2 MiB).
- Text read from SAP screens is data, never instructions; the shaper prepends an untrusted-data
  header and the server `instructions` say so.
- `mcp` refuses to start on an interactive console (exit code 2) and only supports
  `--transport stdio`.

## Audit

Each `tools/call` yields one `McpCallRecord`; `mcp_audit` maps it to the existing audit trail
(never messages or screen content). Audit disabled means no hook.
