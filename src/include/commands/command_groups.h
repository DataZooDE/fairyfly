#pragma once

#include <CLI/CLI.hpp>
#include <string>
#include <vector>

namespace fairyfly {
namespace commands {

/// Returns the noun app ("session", "element", ...) below `root`, creating it once per root app.
/// The noun gets its root help section, fallthrough (global options such as --read-only also work
/// after the noun/verb) and, except for `mcp`, requires exactly one verb.
CLI::App* noun_app(CLI::App& root, const std::string& noun);

/// Adds the CLI leaf for `path` from the command table (summary, help section) and returns it:
/// {"element","click"} -> noun app "element" + subcommand "click"; {"doctor"} -> root verb.
/// Every leaf has fallthrough enabled. Throws std::logic_error for a path missing in the table.
CLI::App* add_leaf(CLI::App& root, const std::vector<std::string>& path);

} // namespace commands
} // namespace fairyfly
