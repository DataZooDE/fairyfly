#pragma once

#include <CLI/CLI.hpp>
#include <string>
#include <memory>
#include <optional>
#include "include/core.h"
#include "include/cli_handler.h"

namespace fairyfly {
namespace commands {

/// Abstract base class for all CLI commands
/// Commands implement setup_cli() to configure CLI11 subcommands
/// and execute() to perform the actual operation
class CommandBase {
public:
    virtual ~CommandBase() = default;

    /// Get the command name (e.g., "attach", "launch", "tcode")
    virtual std::string name() const = 0;

    /// Get command description for help text
    virtual std::string description() const = 0;

    /// Setup CLI11 subcommand with options/flags/arguments
    /// @param app Parent CLI::App to add subcommand to
    /// @return Pointer to created subcommand
    virtual CLI::App* setup_cli(CLI::App& app) = 0;

    /// Execute the command
    /// @param handler CommandHandler for SAP operations
    /// @return Result with status/data/error
    virtual Result execute(cli::CommandHandler& handler) = 0;

    /// Check if this command was parsed from CLI
    /// @return true if command's CLI11 subcommand was selected
    virtual bool was_invoked() const = 0;

    /// Get preferred output format for this command (if any)
    /// @return Output format string ("json", "markdown", "toon") or nullopt to use global format
    virtual std::optional<std::string> get_preferred_output_format() const {
        return std::nullopt;  // Default: no preference, use global format
    }
};

} // namespace commands
} // namespace fairyfly
