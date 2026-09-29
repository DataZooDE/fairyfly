# Development guide

fairyfly is a Windows C++20 CLI for SAP GUI automation through the SAP GUI Scripting COM API. The current command registry is in src/commands/command_registry.cpp. README.md documents supported workflows.

## Build and test

Configure once with CMake and vcpkg, then build only the target needed for the task:

~~~powershell
cmake -S . -B build -G "Visual Studio 17 2022" -DCMAKE_TOOLCHAIN_FILE="$env:VCPKG_ROOT/scripts/buildsystems/vcpkg.cmake"
cmake --build build --config Release --target fairyfly --parallel
cmake --build build --config Release --target unit_tests --parallel
ctest --test-dir build -C Release --output-on-failure
~~~

Visual Studio is a multi-configuration generator: select Release or Debug with --config. Keep the build directory for incremental builds. The Makefile provides Windows build shortcuts. The SAP-dependent scripts under tests/integration/ require a configured SAP GUI session and are separate from the unit tests.

## Code map

- scratch is the place to put temporary output, images or debug scripts
- src/command_table.* is the single table of CLI paths, help sections and MCP tool names/families.
- src/commands/ defines and registers CLI subcommands (command_groups.cpp creates the noun apps, cli_app.cpp builds the tree).
- src/com/ and src/com_automation_engine.cpp handle COM objects and actions.
- src/screen_reader.cpp and src/screen_element_collector.cpp gather screen contents.
- src/formatters/ renders JSON-adjacent data as Markdown or TOON.
- src/connection_manager.cpp handles persisted connection state.
- tests/unit/ uses Catch2.

Use the existing COM wrappers and RAII helpers for new automation work. Preserve structured errors and output formats when changing a command. New live SAP workflows belong in tests/integration/ and should document their required screen and transaction.

## Current boundaries

Since 0.2.0 the CLI is a noun/verb tree defined once in src/command_table.cpp (the single source for CLI paths, help sections, MCP tool names/families and the audit `command`): `session` (list, attach, launch, login, disconnect), `connection list`, `screen` (read, find, capture), `menu` (list, select), `element` (get, click, fill, f4), `key send`, `popup close`, `transaction start`, `credentials` (list, set, delete, import-env), the root verbs `doctor` and `batch` (many commands in one process; lines are argv without the program name, e.g. `["element","click",...]`), and `mcp`. The old flat names (attach, tcode, click, serve, ...) were removed without aliases; see docs/MIGRATION_0.2.md. Global options work before and after the noun/verb. The global --read-only flag (or FAIRYFLY_READ_ONLY=1) refuses state-changing actions; `element click` also supports --doubleclick on grid cells. `mcp` is a stdio MCP server (21 `gui_<noun>_<verb>` tools, see docs/MCP.md and src/mcp/; `mcp tools [--markdown]` prints the table): read-only guard mode by default, `mcp --allow-write` enables write mode, `mcp --tools <families>` limits the exposed tool families, `FAIRYFLY_READ_ONLY=1` is a hard cap, and there is no HTTP transport. Supported output values are json, markdown, and toon. There are no registered connect, profile, or diagnose commands.

`session launch` never reads credentials: it opens the SAP Logon entry through native COM (the optional `--allow-sapshcut` fallback runs `sapshcut -sysname=<name> -maxgui`, never with a password). `session login` takes credentials only from `--credentials-stdin`, `--credentials-file PATH` (deprecated, warns), `--credential NAME`, or, with no flag, the Windows Credential Manager entry named like the saved connection (`credentials set|list|delete|import-env`; targets are `fairyfly:<name>`). There is no implicit `./trial.env` fallback, and no source file under src/ may mention `trial.env`. `session launch <connection> --login [--credential NAME]` runs the same logon through the scripting API after the session is ready (allowed under --read-only: authentication is not a business-state change). The plaintext `trial.env` is git-ignored; migrate it with `fairyfly credentials import-env trial.env --connection Bigfox --delete-file` and rotate the SAP password. `credentials set/import-env` need a console or stdin and return CREDENTIALS_PROMPT_UNAVAILABLE inside `batch`.

Every invocation (and every `batch` line) appends one JSON record to an audit trail, by default `%LOCALAPPDATA%\fairyfly\audit\YYYY-MM.jsonl` (redacted argv, SAP system/client/user/transaction, status, error code, exit code, duration; never error messages, screen content or cell values). Control it with `--no-audit`, `--audit-required`, `FAIRYFLY_AUDIT=0|off|required`, and `FAIRYFLY_AUDIT_FILE`. Append failures never break a command unless audit is required. Set `FAIRYFLY_AUDIT=0` when running unit tests. The audit file is not tamper-proof. MCP tool calls are audited too (`audit_source: "mcp"`, tool, client, request id; never results).

Historical notes under docs/ and at the repository root record earlier investigations and may mention commands or behavior that have changed. Verify current behavior in source or with fairyfly --help before following them.
