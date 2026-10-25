#pragma once

#include "include/commands/command_base.h"

namespace fairyfly {
namespace commands {

/// `fairyfly update check|apply`: new releases from the project's GitHub releases, verified (SHA256SUMS and the
/// Authenticode signer) before the running fairyfly.exe is replaced. See include/update/update_model.h.
class UpdateCommand : public CommandBase {
public:
    CLI::App* setup_cli(CLI::App& app) override;
    Result execute(cli::CommandHandler& handler) override;

private:
    CLI::App* check_cmd_ = nullptr;
    CLI::App* apply_cmd_ = nullptr;
    bool allow_unsigned_ = false;
};

} // namespace commands
} // namespace fairyfly
