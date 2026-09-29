#pragma once

#include "command_base.h"
#include <vector>
#include <memory>
#include <string>

namespace fairyfly {
namespace commands {

/// Singleton registry for all CLI commands
/// Commands register via explicit register_all_commands() call
class CommandRegistry {
public:
    static CommandRegistry& instance() {
        static CommandRegistry registry;
        return registry;
    }

    /// Register a command
    void register_command(std::unique_ptr<CommandBase> command) {
        commands_.push_back(std::move(command));
    }

    /// Drop registrations tied to a previous CLI::App before a new invocation.
    void clear() { commands_.clear(); }

    /// Setup all commands with CLI11
    void setup_all_commands(CLI::App& app) {
        for (auto& command : commands_) {
            command->setup_cli(app);
        }
    }

    /// Find and execute the invoked command
    /// @return Result from executed command, or error if none invoked
    Result execute_active_command(cli::CommandHandler& handler) {
        for (auto& command : commands_) {
            if (command->was_invoked()) {
                return command->execute(handler);
            }
        }

        // No command invoked
        Result result;
        result.status = Result::Status::Error;
        result.error["code"] = "NO_COMMAND";
        result.error["message"] = "No subcommand specified";
        return result;
    }

    /// Get all registered commands (for testing/debugging)
    const std::vector<std::unique_ptr<CommandBase>>& all_commands() const {
        return commands_;
    }

    /// Get output format preference from active command (if any)
    /// @return Preferred format string or nullopt if no preference
    std::optional<std::string> get_active_command_output_format() const {
        for (const auto& command : commands_) {
            if (command->was_invoked()) {
                return command->get_preferred_output_format();
            }
        }
        return std::nullopt;
    }

private:
    CommandRegistry() = default;
    std::vector<std::unique_ptr<CommandBase>> commands_;
};

/// Register all available commands
/// Called explicitly from main() to ensure all commands are registered
void register_all_commands();

} // namespace commands
} // namespace fairyfly
