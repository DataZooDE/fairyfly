# Documentation map

Use the root [README](../README.md) and [CLAUDE.md](../CLAUDE.md) for current commands, build instructions, and implementation boundaries. [BUILD_OPTIMIZATION.md](BUILD_OPTIMIZATION.md) describes the current build setup. [OPEN_WORK.md](OPEN_WORK.md) contains only unfinished investigations and measurements. [ERROR_LOG_AND_IMPROVEMENTS.md](ERROR_LOG_AND_IMPROVEMENTS.md) preserves historical fixes and observations; its older entries are not a todo list. [SEGW_DISPOSABLE_CLEANUP.md](SEGW_DISPOSABLE_CLEANUP.md) gives the verified deletion order for temporary OData V2 services.

The other files in this directory include a SAP GUI scripting reference and dated investigation records. Obsolete setup and diagnostic guides were removed because they described commands that no longer exist or contradicted the current launch workflow. Use the root README and `fairyfly --help` for current commands, and the [integration guide](../tests/integration/README.md) for live test prerequisites.

The EPM generator output still needs a live comparison with a completed result; that exact check is tracked in [open work](OPEN_WORK.md). Its speculative analysis note and one-off capture probe were removed.

Since 0.2.0 the CLI is a noun/verb tree and the MCP tools are named `gui_<noun>_<verb>`; see [MIGRATION_0.2.md](MIGRATION_0.2.md) for the old-to-new tables and [../CHANGELOG.md](../CHANGELOG.md).
