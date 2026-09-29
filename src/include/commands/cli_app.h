#pragma once

#include <CLI/CLI.hpp>
#include <string>

#include "include/commands/global_options.h"

namespace fairyfly {
namespace commands {

/// Adds the global options (--log-level, -v, --output, --verbose-errors, --read-only, --no-audit,
/// --audit-required) and puts them, --help and --version in the "GLOBAL FLAGS" help section.
/// The group apps use fallthrough, so these also work after the noun/verb.
void add_global_options(CLI::App& app, GlobalOptions& options);

/// Builds the whole noun/verb command tree on `app`: allow_windows_style_options(false) (element ids
/// start with '/'), register_all_commands() and setup_all_commands().
void build_command_tree(CLI::App& app);

/// Space-joined names of the parsed subcommand chain after app.parse(): "element click",
/// "mcp tools", "doctor" ("" when no command was parsed). Used as the audit `command`.
std::string invoked_command_path(const CLI::App& app);

} // namespace commands
} // namespace fairyfly
