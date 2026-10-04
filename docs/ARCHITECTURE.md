# Architecture

fairyfly is a single Windows executable (C++20, statically linked) with two faces: a command line program and an MCP
server for AI agents. Both run the same commands through the same code path, so the read-only guard, redaction and
audit trail apply equally to scripts and agents.

## Overview

~~~text
  MCP client (stdio)        MCP client (HTTPS)               script / scheduler
        |                         |                                |
        |                  http.sys (kernel): TLS, timeouts,       |
        |                  URL routing (only /mcp/)                |
        v                         v                                v
  +-------------------------------------------------------------------------------+
  | fairyfly.exe   (unelevated, in the logged-on user's interactive desktop)      |
  |                                                                               |
  |  MCP server: JSON-RPC, protocol eras,       CLI: CLI11 noun/verb tree         |
  |  tokens, scopes, allowlists, rate limits     built from the command table     |
  |              |                                        |                       |
  |              +---------> command registry <-----------+                       |
  |                          (one implementation per command)                     |
  |                                    |                                          |
  |       read-only guard - redaction - status/outcome checks - audit trail       |
  |                                    |                                          |
  |       automation engine: COM wrappers (RAII), screen reader, grid/tree/table  |
  |       extraction, formatters (JSON, Markdown, TOON)                           |
  +-------------------------------------------------------------------------------+
                                       |  SAP GUI Scripting API (COM, out of process)
                                       v
                       SAP GUI for Windows (saplogon.exe)  --->  SAP system
~~~

## Components

| Area | Source | Role |
|---|---|---|
| Entry point | `src/main.cpp`, `src/cli_entry.cpp` | Converts the Windows command line and runs the CLI. `main.cpp` is tiny; everything else lives in the `fairyfly_core` library shared with the tests. |
| Command table | `src/command_table.cpp` | The single list of CLI paths, help sections, MCP tool names, tool families and audit command names. |
| Commands | `src/commands/` | One class per command: options (`setup_cli`, including help examples) and execution. `command_groups.cpp` creates the noun groups, `cli_app.cpp` builds the tree. |
| Command handler | `src/cli_handler*.cpp` | Shared execution: connection resolution, read-only guard, redaction, result envelope, audit. |
| COM layer | `src/com/`, `src/com_automation_engine*.cpp` | RAII wrappers around SAP GUI COM objects (application, connection, session, window, element), cached DISPIDs, actions. |
| Screen reading | `src/screen_reader.cpp`, `src/bulk_screen_reader.cpp`, `src/object_tree.cpp`, `src/screen_element_collector.cpp`, `src/grid_analyzer.cpp`, `src/table_data_extractor.cpp`, `src/html_viewer_reader.cpp` | Turn a SAP window into structured data (see below). |
| Formatters | `src/formatters/` | JSON, Markdown and TOON rendering of results. |
| Safety | `src/read_only_guard.cpp`, `src/sensitive_data.cpp`, `src/action_status.cpp` | Refuse state-changing actions in read-only mode, redact secrets, judge status bar outcomes. |
| Sessions and logon | `src/connection_manager.cpp`, `src/connection_launcher.cpp`, `src/login_flow.cpp`, `src/credential_store.cpp` | Saved connections, SAP Logon launch, logon through scripting, Credential Manager access. |
| MCP server | `src/mcp/` | Protocol, tool catalog, dispatcher, policy, HTTP endpoint, session routing. |
| Authentication | `src/auth/` | Bearer tokens: creation, hashing, storage, scopes, IP rules, authorization. |
| Machine setup | `src/setup/` | `mcp setup`, `teardown`, `doctor`: certificate, URL reservation, TLS binding, firewall, elevation. |
| Configuration | `src/config/` | `mcp.yaml`, client config snippets, `mcp doctor`. |
| Tray | `src/tray/` | Notification-area icon, menu and autostart. |
| Audit | `src/audit_log.cpp`, `src/mcp/mcp_audit.cpp` | One JSON line per command or tool call. |

## How a screen is read

SAP GUI Scripting is an out-of-process COM API: every property read is a round trip to `saplogon.exe`. Reading
speed is therefore mostly about making fewer calls.

1. **Bulk reader (default).** One `GuiSession.GetObjectTree` call (SAP GUI 7.70 PL3 or later) returns the whole
   control tree with the needed properties. A few properties are still read per element: `AccLabel`, and `Selected`,
   which must never be requested in bulk because the call then fails with a server fault that can take SAP GUI down.
2. **Per-element reader (fallback).** Walks `Children` and probes known `FindById` paths that `Children` misses. It
   is used when the bulk call is unavailable or fails (the reason is in `diagnostics.screen_reader`), for
   `--probe-all`, and for `screen find`. `FAIRYFLY_SCREEN_READER=auto|bulk|legacy` selects the reader.
3. **Analysis.** Grids (`GuiGridView`), table controls, classic lists (`GuiUserArea`), trees, tab strips, text
   editors and HTML viewers get dedicated extractors (paging, header detection, empty-row trimming).
4. **Shaping.** The semantic classifier labels elements; the redaction rules mask secrets; the formatter renders
   JSON, Markdown or TOON. MCP results are capped in size and start with an "untrusted data" header.

## Actions and outcomes

Clicks, fills, keys, menu selections and transaction starts run through the command handler. Before an action it
applies the read-only guard. After the action it reads the status bar: information, success and warning messages
count as success, error and abort messages (types E and A) as failure. This lets the agent see when SAP rejected an
action. Tab-dependent operations restore the original tab afterwards (`tab_guard`).

## The MCP server

`fairyfly mcp` does not reimplement commands. Each tool is described by a `ToolSpec` (name `gui_<noun>_<verb>`, JSON
schema, family, write flag) that maps tool arguments to CLI arguments. The dispatcher then runs them through a
private copy of the normal command registry on the shared handler, the same way as `batch`.

| Module (`src/mcp/`) | Role |
|---|---|
| `json_rpc`, `transport`, `server` | JSON-RPC 2.0 framing; stdio transport (newline-delimited, stdout carries only protocol messages); `initialize`, `ping`, `tools/list`, `tools/call`, cancellation, queue and timeout |
| `tool_catalog`, `tool_catalog_write` | Read and write tool specs generated from the command table; per-principal tool descriptions |
| `dispatcher`, `call_executor` | Tool call to command invocation; one call at a time on the COM thread |
| `policy` | Read-only and write mode, tool visibility, rate limiting |
| `result_shaper` | Command result to MCP result: size caps, image handling, untrusted-data header, tool-call hints |
| `http_endpoint`, `http_sys_server`, `http_auth` | `POST /mcp` over http.sys, protocol eras (stateless 2026-07-28 and legacy 2025-11-25/2025-06-18), optional SSE, bearer authentication |
| `session_*` | Owner-mode routing of calls to several SAP GUI windows: worker processes, per-window lanes, leases |
| `mcp_audit` | Maps each call to an audit record |

**Threading.** COM objects of SAP GUI live in a single-threaded apartment. The main thread owns COM and the command
handler, and every tool call runs there, one at a time (FIFO executor queue of 16, soft timeout of 120 s). A
watchdog answers a call that passes the timeout with `CALL_TIMEOUT` right away, but the call cannot be interrupted
inside COM and keeps running; until it finishes, new calls get `SERVER_BUSY`. A timed-out action may therefore still
complete, and clients must not simply repeat it. A reader thread (stdio) or 16 HTTP
receive workers (http.sys, listener queue of 256) only parse, authenticate and queue requests. Because calls are
serialized, the per-token check, invoke and update sequence is atomic per process. The exception is owner mode
below, where calls for different SAP windows run in separate worker processes.

**Parallel SAP windows (owner mode).** With `owner.sap_identities` configured, the tray routes calls for different
SAP GUI windows to private worker processes with ordered per-window lanes, so different windows can make progress
at the same time while calls to one window stay ordered. State-changing calls need a session lease. See
[MCP_REMOTE.md](MCP_REMOTE.md) and [MCP_SAP_AUTHENTICATION.md](MCP_SAP_AUTHENTICATION.md).

## Remote access and http.sys

fairyfly listens through the Windows HTTP Server API (http.sys) and needs no IIS, reverse proxy, OpenSSL or other
runtime. TLS is terminated in the kernel with a certificate from `LocalMachine\My`, so the private key never enters
the fairyfly process, which runs unelevated. An administrator step (`mcp setup`, one UAC prompt) creates the
certificate, reserves the URL for one Windows user and binds the certificate to the port. The running server holds
no key and no administrator rights. The client address used for allowlists is always the socket peer; forwarded
headers are ignored. Details: [MCP_SETUP.md](MCP_SETUP.md), [SECURITY.md](SECURITY.md).

## Why not a Windows service

SAP GUI Scripting talks to the SAP GUI process of a logged-on user and needs that user's interactive desktop.
Windows services run in Session 0, which has no access to that desktop. fairyfly is therefore an ordinary process: a
console program, or with `--tray` a hidden console plus a notification-area icon, optionally started at logon. The
tray UI runs on its own thread with a message pump and talks to the server only through `IServerControl`; the COM
executor stays on the main thread. Every operating system effect of the tray (icon, registry, processes, files,
mutex, dialogs, clipboard) sits behind an interface with fakes in the unit tests. See [MCP_TRAY.md](MCP_TRAY.md).

## State and storage

fairyfly has no database and keeps little state:

| State | Where | Notes |
|---|---|---|
| Saved connections | `%LOCALAPPDATA%\fairyfly\sessions\` | Which SAP GUI session a connection id refers to; validated against the live session before use |
| SAP credentials | Credential Manager `fairyfly:<name>` | Read only by `session login` / `session launch --login` |
| Access tokens | Credential Manager `fairyfly-mcp:<name>` | SHA-256 hash and restrictions only, never the secret |
| Audit trail | `%LOCALAPPDATA%\fairyfly\audit\YYYY-MM.jsonl` | Append-only by convention |
| Configuration | `%LOCALAPPDATA%\fairyfly\mcp.yaml`, `mcp-setup.json` | Server settings; record of what `mcp setup` created |
| Runtime state | Memory | Sticky default connection per principal, leases, rate budgets, T-code allowlist flags; lost on restart |

## Tests

- `tests/unit/`: Catch2 unit tests with fakes for COM and the operating system. They run without SAP (`ctest`).
- `tests/integration/`: PowerShell and Python scripts against a live SAP GUI session, including the MCP stdio and
  HTTP smoke tests. Each script documents the screen and transaction it needs.

See [BUILDING.md](BUILDING.md) for build and test commands.
