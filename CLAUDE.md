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
- src/commands/ defines and registers CLI subcommands.
- src/com/ and src/com_automation_engine.cpp handle COM objects and actions.
- src/screen_reader.cpp and src/screen_element_collector.cpp gather screen contents.
- src/formatters/ renders JSON-adjacent data as Markdown or TOON.
- src/connection_manager.cpp handles persisted connection state.
- tests/unit/ uses Catch2.

Use the existing COM wrappers and RAII helpers for new automation work. Preserve structured errors and output formats when changing a command. New live SAP workflows belong in tests/integration/ and should document their required screen and transaction.

## Current boundaries

The CLI offers attach, launch, disconnect, connections, list, tcode, click, fill, get, screen (read, find, menu, capture), press_f4, send-key, close, batch (many commands in one process), login, credentials (set, list, delete, import-env), and doctor; the global --read-only flag (or FAIRYFLY_READ_ONLY=1) refuses state-changing actions; click also supports --doubleclick on grid cells. `serve` is a stdio MCP server (20 `sap_*` tools, see docs/MCP.md and src/mcp/): read-only guard mode by default, `serve --allow-write` enables write mode, `FAIRYFLY_READ_ONLY=1` is a hard cap, and there is no HTTP transport. Supported output values are json, markdown, and toon. There are no registered connect, profile, session, or diagnose commands.

`launch` never reads credentials: it opens the SAP Logon entry through native COM (the optional `--allow-sapshcut` fallback runs `sapshcut -sysname=<name> -maxgui`, never with a password). `login` takes credentials only from `--credentials-stdin`, `--credentials-file PATH` (deprecated, warns), `--credential NAME`, or, with no flag, the Windows Credential Manager entry named like the saved connection (`credentials set|list|delete|import-env`; targets are `fairyfly:<name>`). There is no implicit `./trial.env` fallback, and no source file under src/ may mention `trial.env`. `launch <connection> --login [--credential NAME]` runs the same logon through the scripting API after the session is ready (allowed under --read-only: authentication is not a business-state change). The plaintext `trial.env` is git-ignored; migrate it with `fairyfly credentials import-env trial.env --connection Bigfox --delete-file` and rotate the SAP password. `credentials set/import-env` need a console or stdin and return CREDENTIALS_PROMPT_UNAVAILABLE inside `batch`.

Every invocation (and every `batch` line) appends one JSON record to an audit trail, by default `%LOCALAPPDATA%\fairyfly\audit\YYYY-MM.jsonl` (redacted argv, SAP system/client/user/transaction, status, error code, exit code, duration; never error messages, screen content or cell values). Control it with `--no-audit`, `--audit-required`, `FAIRYFLY_AUDIT=0|off|required`, and `FAIRYFLY_AUDIT_FILE`. Append failures never break a command unless audit is required. Set `FAIRYFLY_AUDIT=0` when running unit tests. The audit file is not tamper-proof. MCP tool calls are audited too (`audit_source: "mcp"`, tool, client, request id; never results).

Historical notes under docs/ and at the repository root record earlier investigations and may mention commands or behavior that have changed. Verify current behavior in source or with fairyfly --help before following them.
