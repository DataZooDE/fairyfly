# fairyfly documentation

New to fairyfly? Start with the [project README](../README.md): what fairyfly is and a five-minute quick start.

## Use fairyfly

| Guide | For | What it covers |
|---|---|---|
| [SETUP.md](SETUP.md) | Everyone | Requirements, enabling SAP GUI Scripting, first connection, credentials, troubleshooting |
| [MCP.md](MCP.md) | Agent builders | Connecting Claude Code, Claude Desktop and other MCP clients; the `gui_*` tools; read-only and write mode; access tokens and scopes |
| [CLI.md](CLI.md) | Script authors | Every command, screen reading options, batch mode, read-only guard, credentials, audit trail, migration from the old command names |

## Run fairyfly for remote agents

| Guide | What it covers |
|---|---|
| [MCP_REMOTE.md](MCP_REMOTE.md) | Serving MCP over HTTPS from a Windows machine: step-by-step setup, tokens, client cookbook, operations, troubleshooting, Linux check list |
| [MCP_SETUP.md](MCP_SETUP.md) | Reference for `mcp setup`, `mcp teardown`, `mcp cert export` and the http.sys checks of `mcp doctor` |
| [MCP_TRAY.md](MCP_TRAY.md) | Tray mode, autostart, `mcp.yaml` configuration, client config snippets, `mcp doctor` |
| [MCP_SAP_AUTHENTICATION.md](MCP_SAP_AUTHENTICATION.md) | Several SAP users behind one endpoint; password, SSO and SNC logon |
| [LINK.md](LINK.md) | `fairyfly link`: pairing this desktop with the fairyfly cloud gateway (QR code), serving its AI assistants outbound-only, audit and approvals |

## Understand fairyfly

| Guide | What it covers |
|---|---|
| [SECURITY.md](SECURITY.md) | Security principles and layers, credentials, data handling, prompt injection, threat model, production recommendations, release integrity |
| [ARCHITECTURE.md](ARCHITECTURE.md) | Components, how screens are read, the MCP server, threading, http.sys, storage |
| [DATA_DIR.md](DATA_DIR.md) | The per-user data folder `%USERPROFILE%\.fairyfly`, what is in it, and the move from `%LOCALAPPDATA%\fairyfly` |

## Develop fairyfly

| Guide | What it covers |
|---|---|
| [BUILDING.md](BUILDING.md) | Toolchain, build, tests, Makefile, icons, versions and releases |
| [SIGNING.md](SIGNING.md) | Code signing options and the release procedure |
| [MSIX.md](MSIX.md) | The Microsoft Store (MSIX) build option: no self-update, StartupTask, manifest template, what MSIX virtualizes and what is left |
| [../tests/integration/README.md](../tests/integration/README.md) | Live SAP integration scripts and their prerequisites |
| [../CHANGELOG.md](../CHANGELOG.md) | User-visible changes per version |
| [examples/](examples/) | Example MCP client configurations |

## Internal notes

[internal/](internal/) holds working material of the development team: open work and its archive, the error and
fix history, build measurements, a general SAP GUI Scripting API reference and dated investigation notes. Plans
and traces of work in progress (`MCP_PARALLEL_SESSIONS_PLAN.md`, `AGENT_CLI_IMPROVEMENT_PLAN.md`, `traces/`) are
also kept here for now. These files record how things were at the time they were written and may describe commands
or behavior that have changed. The guides above and `fairyfly --help` describe the current version.
