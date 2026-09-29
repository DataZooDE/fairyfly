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

The CLI offers attach, launch, disconnect, connections, list, tcode, click, fill, get, screen (read, find, menu, capture), press_f4, send-key, close, batch (many commands in one process), and doctor; the global --read-only flag (or FAIRYFLY_READ_ONLY=1) refuses state-changing actions; click also supports --doubleclick on grid cells. The serve command is registered but returns NOT_IMPLEMENTED; there is no MCP server yet. Supported output values are json, markdown, and toon. There are no registered connect, profile, session, or diagnose commands.

The launch path can read plaintext credentials from trial.env and passes them to sapshcut. Keep that file private and out of version control. Credential Manager integration and an audit trail are not implemented.

Historical notes under docs/ and at the repository root record earlier investigations and may mention commands or behavior that have changed. Verify current behavior in source or with fairyfly --help before following them.
