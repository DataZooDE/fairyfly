#pragma once
// Phase-4 additions of `fairyfly mcp`: -c/--config, --tray/--install-autostart/--remove-autostart and the
// subcommands `config show|path|init|validate`, `client-config`, `doctor`. Kept out of mcp_command.cpp so
// parallel phases (HTTP flags, token, iis) only need to touch single hook lines there.

#include <CLI/CLI.hpp>
#include <functional>
#include <optional>
#include <string>

#include "include/cli_handler.h"
#include "include/commands/global_options.h"
#include "include/config/client_config.h"
#include "include/config/mcp_config.h"
#include "include/mcp/types.h"

namespace fairyfly::commands {

struct McpExtras {
    CLI::App* mcp_app = nullptr;
    // options of the mcp app
    std::string config_path;   ///< -c/--config
    bool tray = false;
    bool install_autostart = false;
    bool remove_autostart = false;
    // subcommands
    CLI::App* config_show = nullptr;
    CLI::App* config_path_cmd = nullptr;
    CLI::App* config_init = nullptr;
    CLI::App* config_validate = nullptr;
    CLI::App* client_config = nullptr;
    CLI::App* doctor = nullptr;
    bool init_force = false;
    std::string output = "text";   ///< text | json | markdown (each leaf has its own --output bound here)
    config::ClientConfigOptions client;
};

/// Registers the options/subcommands on the `mcp` app.
void setup_mcp_extras(CLI::App& mcp_app, McpExtras& extras);

using HandlerProvider = std::function<cli::CommandHandler&()>;

/// Runs whatever the extras own: a subcommand, autostart install/remove or the tray. Returns the exit
/// code when the invocation was handled. Returns nullopt for the plain server path, after applying the
/// YAML/env layers to `options` (flag > env > yaml > default). Config errors return exit code 2.
std::optional<int> run_mcp_extras(McpExtras& extras, mcp::ServeOptions& options, const HandlerProvider& get_handler,
                                  const GlobalOptions& global);

// ---- shared helpers (mcp_config_command.cpp) ------------------------------------------------------
struct LoadedConfig {
    config::ConfigPath path;
    bool exists = false;
    bool read_error = false;
    config::ParseResult parsed;
};
LoadedConfig load_config(const std::string& flag_path, const config::EnvLookup& env);
/// The flag layer: only options the user really typed (CLI11 counts), parsed with the key table.
config::McpConfig flags_layer(const CLI::App& mcp_app);
/// Prints an error as text (stderr) or JSON (stdout) and returns `exit_code`.
int report_error(const std::string& output, const std::string& code, const std::string& message, int exit_code = 1);

// ---- other subcommands ---------------------------------------------------------------------------
int run_client_config_command(McpExtras& extras);
int run_mcp_doctor_command(McpExtras& extras, const HandlerProvider& get_handler);

} // namespace fairyfly::commands
