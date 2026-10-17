#pragma once

#include "include/commands/command_base.h"
#include "include/link/link_runner.h"

namespace fairyfly {
namespace commands {

/// `fairyfly link pair|status|run|unpair` (D10): the cloud gateway link. `link run` is started from cli_entry.cpp
/// (like `mcp`) because it needs the shared handler and the audit sink; the others run through execute().
class LinkCommand : public CommandBase {
public:
    std::string name() const override { return "link"; }
    std::string description() const override { return "Connect this desktop to the fairyfly cloud gateway (outbound only)"; }
    CLI::App* setup_cli(CLI::App& app) override;
    Result execute(cli::CommandHandler& handler) override;
    bool was_invoked() const override { return cmd_ && *cmd_; }
    bool run_invoked() const { return run_cmd_ && *run_cmd_; }
    /// pair, status, pause, resume, unpair: they never touch SAP, so cli_entry.cpp runs them without the shared
    /// CommandHandler (creating it attaches to SAP GUI over COM, which can stall while SAP GUI is busy).
    Result execute_without_sap();
    /// Parsed options (valid after parsing); --tools is folded into families.
    link::LinkOptions options() const;

private:
    CLI::App* cmd_ = nullptr;
    CLI::App* pair_cmd_ = nullptr;
    CLI::App* status_cmd_ = nullptr;
    CLI::App* run_cmd_ = nullptr;
    CLI::App* unpair_cmd_ = nullptr;
    CLI::App* pause_cmd_ = nullptr;
    CLI::App* resume_cmd_ = nullptr;
    link::LinkOptions options_;
    std::string tools_filter_;
};

} // namespace commands
} // namespace fairyfly
