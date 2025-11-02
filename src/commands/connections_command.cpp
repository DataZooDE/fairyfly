#include "include/commands/command_base.h"


namespace fairyfly {
namespace commands {

class ConnectionsCommand : public CommandBase {
public:
    std::string name() const override { return "connections"; }

    std::string description() const override {
        return "List and manage SAP connections";
    }

    CLI::App* setup_cli(CLI::App& app) override {
        cmd_ = app.add_subcommand(name(), description());
        cmd_->add_flag("--cleanup", cleanup_, "Remove invalid connection files");
        return cmd_;
    }

    Result execute(cli::CommandHandler& handler) override {
        return handler.handle_connections_list(cleanup_);
    }

    bool was_invoked() const override {
        return cmd_ && *cmd_;
    }

private:
    CLI::App* cmd_ = nullptr;
    bool cleanup_ = false;
};

// Factory function
// Factory function
std::unique_ptr<CommandBase> create_connections_command() {
    return std::make_unique<ConnectionsCommand>();
}

} // namespace commands
} // namespace fairyfly
