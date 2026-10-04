#include "include/commands/command_base.h"

#include <optional>

namespace fairyfly {
namespace commands {

class CloseCommand : public CommandBase {
public:
    std::string name() const override { return "popup close"; }

    std::string description() const override {
        return "Close the active popup (sends F12 to wnd[N>0]) and verify it closed";
    }

    CLI::App* setup_cli(CLI::App& app) override {
        cmd_ = add_leaf(app, {"popup","close"});
        cmd_->footer(R"HELP(Usage notes:
Cancels the active popup and verifies it closed. Read the popup first to decide whether
cancelling is appropriate; this does not close the main SAP window.
Example: fairyfly popup close --connection 3
)HELP");
        cmd_->add_option("--vkey", vkey_, "SAP VKey to send to the popup (default 12 = F12/Cancel)")
            ->default_val(12)
            ->check(CLI::Range(0, 99));
        cmd_->add_option("--connection", conn_id_, "Connection ID to use");
        return cmd_;
    }

    Result execute(cli::CommandHandler& handler) override {
        return handler.handle_close(vkey_, conn_id_);
    }

    bool was_invoked() const override {
        return cmd_ && *cmd_;
    }

private:
    CLI::App* cmd_ = nullptr;
    int vkey_ = 12;
    std::optional<int> conn_id_;
};

std::unique_ptr<CommandBase> create_close_command() {
    return std::make_unique<CloseCommand>();
}

} // namespace commands
} // namespace fairyfly
