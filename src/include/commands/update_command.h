#pragma once

#include "include/commands/command_base.h"

namespace fairyfly {
namespace commands {

/// `fairyfly update check|apply`: new releases from the project's GitHub releases, verified (SHA256SUMS and the
/// Authenticode signer) before the running fairyfly.exe is replaced. See include/update/update_model.h.
class UpdateCommand : public CommandBase {
public:
    std::string name() const override { return "update"; }
    std::string description() const override { return "Check for and install new fairyfly releases"; }
    CLI::App* setup_cli(CLI::App& app) override;
    Result execute(cli::CommandHandler& handler) override;
    bool was_invoked() const override { return cmd_ && *cmd_; }

private:
    CLI::App* cmd_ = nullptr;
    CLI::App* check_cmd_ = nullptr;
    CLI::App* apply_cmd_ = nullptr;
    bool allow_unsigned_ = false;
};

} // namespace commands
} // namespace fairyfly
