#pragma once

#include "include/commands/command_base.h"
#include "include/mcp/types.h"

namespace fairyfly {
namespace commands {

/// `fairyfly serve`: MCP server over stdio. The run loop is started from cli_entry.cpp
/// (mcp::run_serve) because, like `batch`, it needs the shared handler and must copy the options
/// before the registry is rebuilt.
class ServeCommand : public CommandBase {
public:
    std::string name() const override { return "serve"; }
    std::string description() const override { return "Start the MCP server (stdio)"; }

    CLI::App* setup_cli(CLI::App& app) override;
    Result execute(cli::CommandHandler& handler) override;
    bool was_invoked() const override { return cmd_ && *cmd_; }

    /// Parsed options (valid after parsing); --default-connection is folded in.
    mcp::ServeOptions options() const;

private:
    CLI::App* cmd_ = nullptr;
    mcp::ServeOptions options_;
    int default_connection_ = -1;
};

} // namespace commands
} // namespace fairyfly
