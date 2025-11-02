#include "include/commands/command_registry.h"
#include "include/commands/command_base.h"

// Forward declarations of all command classes
namespace fairyfly {
namespace commands {

// Command factory functions (defined in each command file)
std::unique_ptr<CommandBase> create_attach_command();
std::unique_ptr<CommandBase> create_launch_command();
std::unique_ptr<CommandBase> create_disconnect_command();
std::unique_ptr<CommandBase> create_connections_command();
std::unique_ptr<CommandBase> create_tcode_command();
std::unique_ptr<CommandBase> create_click_command();
std::unique_ptr<CommandBase> create_fill_command();
std::unique_ptr<CommandBase> create_get_command();
std::unique_ptr<CommandBase> create_list_command();
std::unique_ptr<CommandBase> create_screen_command();
std::unique_ptr<CommandBase> create_serve_command();

void register_all_commands() {
    auto& registry = CommandRegistry::instance();

    // Register all commands explicitly
    registry.register_command(create_attach_command());
    registry.register_command(create_launch_command());
    registry.register_command(create_disconnect_command());
    registry.register_command(create_connections_command());
    registry.register_command(create_tcode_command());
    registry.register_command(create_click_command());
    registry.register_command(create_fill_command());
    registry.register_command(create_get_command());
    registry.register_command(create_list_command());
    registry.register_command(create_screen_command());
    registry.register_command(create_serve_command());
}

} // namespace commands
} // namespace fairyfly
