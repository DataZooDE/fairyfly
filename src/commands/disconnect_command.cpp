#include "include/commands/command_base.h"

#include <optional>

namespace fairyfly {
namespace commands {

class DisconnectCommand : public CommandBase {
public:
    std::string name() const override { return "disconnect"; }

    std::string description() const override {
        return "Disconnect from SAP system";
    }

    CLI::App* setup_cli(CLI::App& app) override {
        cmd_ = app.add_subcommand(name(), description());
        cmd_->add_option("--connection", conn_id_, "Connection ID to disconnect");
        return cmd_;
    }

    Result execute(cli::CommandHandler& handler) override {
        return handler.handle_disconnect(conn_id_);
    }

    bool was_invoked() const override {
        return cmd_ && *cmd_;
    }

private:
    CLI::App* cmd_ = nullptr;
    std::optional<int> conn_id_;
};

// Factory function
// Factory function
std::unique_ptr<CommandBase> create_disconnect_command() {
    return std::make_unique<DisconnectCommand>();
}

} // namespace commands
} // namespace fairyfly
