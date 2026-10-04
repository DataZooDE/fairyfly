#include "include/commands/command_base.h"


namespace fairyfly {
namespace commands {

class ConnectionsCommand : public CommandBase {
public:
    std::string name() const override { return "connection list"; }

    std::string description() const override {
        return "List and manage SAP connections";
    }

    CLI::App* setup_cli(CLI::App& app) override {
        cmd_ = add_leaf(app, {"connection","list"});
        cmd_->footer(R"HELP(Usage notes:
Lists saved Fairyfly connections. Use a live saved numeric ID as --connection.
CONNECTION_NOT_FOUND means the saved file is missing; INVALID_CONNECTION can mean its SAP session ended.
Recover with session list and session attach --session-id using a currently live session.
Example: fairyfly connection list
)HELP");
        cmd_->add_flag("--cleanup", cleanup_, "Remove invalid connection files");
        add_output_option(cmd_, output_format_);
        return cmd_;
    }

    Result execute(cli::CommandHandler& handler) override {
        return handler.handle_connections_list(cleanup_);
    }

    bool was_invoked() const override {
        return cmd_ && *cmd_;
    }

    std::optional<std::string> get_preferred_output_format() const override {
        return output_override(output_format_);
    }

private:
    CLI::App* cmd_ = nullptr;
    bool cleanup_ = false;
    std::string output_format_;
};

// Factory function
// Factory function
std::unique_ptr<CommandBase> create_connections_command() {
    return std::make_unique<ConnectionsCommand>();
}

} // namespace commands
} // namespace fairyfly
