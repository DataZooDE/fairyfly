#pragma once

#include "include/commands/command_base.h"
#include "include/packaging.h"

namespace fairyfly {
namespace commands {

/// `fairyfly update check|apply`: new releases from the project's GitHub releases, verified (SHA256SUMS and the
/// Authenticode signer) before the running fairyfly.exe is replaced. See include/update/update_model.h.
/// In an MSIX build (packaging::self_updates false) both verbs answer UPDATE_MANAGED_BY_STORE: the Store updates it.
class UpdateCommand : public CommandBase {
public:
    explicit UpdateCommand(packaging::Package package = packaging::kPackage) : package_(package) {}
    CLI::App* setup_cli(CLI::App& app) override;
    Result execute(cli::CommandHandler& handler) override;

private:
    CLI::App* check_cmd_ = nullptr;
    CLI::App* apply_cmd_ = nullptr;
    bool allow_unsigned_ = false;
    packaging::Package package_;
};

} // namespace commands
} // namespace fairyfly
