#include "include/commands/command_base.h"

namespace fairyfly {
namespace commands {

class LaunchCommand : public CommandBase {
public:
    std::string name() const override { return "launch"; }

    std::string description() const override {
        return "Launch SAP Logon connection";
    }

    CLI::App* setup_cli(CLI::App& app) override {
        cmd_ = app.add_subcommand(name(), description());
        cmd_->add_option("connection", connection_name_, "Connection name (e.g., PRD, DEV)")
            ->required();
        cmd_->add_flag("--allow-sapshcut", allow_sapshcut_,
            "Allow sapshcut to open a SAP Logon entry when native COM cannot; log in separately");
        cmd_->add_flag("--login", login_,
            "After launching, log in with stored credentials (Credential Manager entry named like the connection)");
        cmd_->add_option("--credential", credential_name_,
            "With --login: stored credential name (default: the connection name)");
        return cmd_;
    }

    Result execute(cli::CommandHandler& handler) override {
        if (!login_ && !credential_name_.empty()) {
            Result result;
            result.status = Result::Status::Error;
            result.error = {{"code", "INVALID_ARGUMENT"},
                            {"message", "--credential requires --login"}};
            return result;
        }
        return handler.handle_launch(connection_name_, allow_sapshcut_, login_, credential_name_);
    }

    bool was_invoked() const override {
        return cmd_ && *cmd_;
    }

private:
    CLI::App* cmd_ = nullptr;
    std::string connection_name_;
    bool allow_sapshcut_ = false;
    bool login_ = false;
    std::string credential_name_;
};

// Factory function
std::unique_ptr<CommandBase> create_launch_command() {
    return std::make_unique<LaunchCommand>();
}

} // namespace commands
} // namespace fairyfly
