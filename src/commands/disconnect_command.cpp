#include "include/commands/command_base.h"

#include <optional>

namespace fairyfly {
namespace commands {

class DisconnectCommand : public CommandBase {
public:
    std::string name() const override { return "session disconnect"; }

    std::string description() const override {
        return "Remove a saved connection; optionally close its SAP GUI session";
    }

    CLI::App* setup_cli(CLI::App& app) override {
        cmd_ = add_leaf(app, {"session","disconnect"});
        cmd_->footer(R"HELP(Usage notes:
Removes the saved Fairyfly connection. --close-session also closes its SAP GUI session.
Use only when the task calls for disconnecting; preserve sessions used by other work.
Example: fairyfly session disconnect --connection 3
)HELP");
        cmd_->add_option("--connection", conn_id_, "Connection ID to disconnect");
        cmd_->add_flag("--close-session", close_session_, "Close the SAP GUI session before removing its saved connection");
        return cmd_;
    }

    Result execute(cli::CommandHandler& handler) override {
        return handler.handle_disconnect(conn_id_, close_session_);
    }

    bool was_invoked() const override {
        return cmd_ && *cmd_;
    }

private:
    CLI::App* cmd_ = nullptr;
    std::optional<int> conn_id_;
    bool close_session_ = false;
};

// Factory function
// Factory function
std::unique_ptr<CommandBase> create_disconnect_command() {
    return std::make_unique<DisconnectCommand>();
}

} // namespace commands
} // namespace fairyfly
