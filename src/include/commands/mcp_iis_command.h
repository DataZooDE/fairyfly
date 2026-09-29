#pragma once

#include <CLI/CLI.hpp>

#include "include/core.h"

namespace fairyfly {
namespace commands {

/// Adds `iis` (with setup, status, remove) below the `mcp` app. Called from McpCommand::setup_cli.
/// The parsed options live in this module (reset on every call: the registry is rebuilt per invocation).
void register_mcp_iis_cli(CLI::App& mcp_app);

/// True when `mcp iis <verb>` was selected on the command line.
bool mcp_iis_invoked();

/// Runs the selected verb against the real machine (PowerShell host, Credential Manager). Never
/// reached by unit tests, which drive iis::run_setup/run_status/run_remove with fakes.
Result mcp_iis_execute();

} // namespace commands
} // namespace fairyfly
