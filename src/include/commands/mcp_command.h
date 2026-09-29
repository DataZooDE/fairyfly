#pragma once

#include "include/commands/command_base.h"
#include "include/commands/mcp_extras.h"
#include "include/mcp/types.h"

namespace fairyfly {
namespace commands {

/// `fairyfly mcp`: starts the MCP server (stdio by default); `fairyfly mcp tools [--markdown]`
/// prints the tool table. The `mcp` app has an OPTIONAL subcommand (later phases add token, iis,
/// config, ...); without one the default action is to run the server. The run loop is started from
/// cli_entry.cpp (mcp::run_mcp) because, like `batch`, it needs the shared handler and must copy
/// the options before the registry is rebuilt.
class McpCommand : public CommandBase {
public:
    std::string name() const override { return "mcp"; }
    std::string description() const override { return "Start the MCP server (stdio)"; }

    CLI::App* setup_cli(CLI::App& app) override;
    Result execute(cli::CommandHandler& handler) override;
    bool was_invoked() const override { return cmd_ && *cmd_; }

    /// True when the `tools` subcommand was selected.
    bool tools_invoked() const { return tools_cmd_ && *tools_cmd_; }
    bool tools_markdown() const { return tools_markdown_; }

    /// Parsed options (valid after parsing); --default-connection and --tools are folded in.
    mcp::ServeOptions options() const;

    /// Phase-4 state: -c/--config, --tray, config/client-config/doctor subcommands (mcp_extras.h).
    McpExtras& extras() { return extras_; }

private:
    CLI::App* cmd_ = nullptr;
    CLI::App* tools_cmd_ = nullptr;
    mcp::ServeOptions options_;
    int default_connection_ = -1;
    std::string tools_filter_;
    bool tools_markdown_ = false;
    McpExtras extras_;
};

} // namespace commands
} // namespace fairyfly
