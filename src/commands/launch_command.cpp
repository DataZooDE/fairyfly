#include "include/commands/command_base.h"

namespace fairyfly {
namespace commands {

class LaunchCommand : public CommandBase {
public:
    std::string name() const override { return "session launch"; }

    std::string description() const override {
        return "Launch SAP Logon connection";
    }

    CLI::App* setup_cli(CLI::App& app) override {
        cmd_ = add_leaf(app, {"session","launch"});
        cmd_->add_option("connection", connection_name_, "Connection name (e.g., PRD, DEV)")
            ->required();
        cmd_->add_flag("--allow-sapshcut", allow_sapshcut_,
            "Allow sapshcut to open a SAP Logon entry when native COM cannot; log in separately");
        cmd_->add_flag("--login", login_,
            "After launching, log in with stored credentials (Credential Manager entry named like the connection)");
        cmd_->add_option("--credential", credential_name_,
            "With --login: stored credential name (default: the connection name)");
        cmd_->add_option("--multiple-logon", multiple_logon_,
            "With --login: if the user is already logged on: fail (default), keep, terminate, or end "
            "(DESTRUCTIVE: ends the user's other logons; refused under --read-only)")
            ->check(CLI::IsMember({"fail", "keep", "end", "terminate"}));
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
        if (!login_ && !multiple_logon_.empty()) {
            Result result;
            result.status = Result::Status::Error;
            result.error = {{"code", "INVALID_ARGUMENT"},
                            {"message", "--multiple-logon requires --login"}};
            return result;
        }
        return handler.handle_launch(connection_name_, allow_sapshcut_, login_, credential_name_,
                                     multiple_logon_.empty() ? "fail" : multiple_logon_);
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
    std::string multiple_logon_;
};

// Factory function
std::unique_ptr<CommandBase> create_launch_command() {
    return std::make_unique<LaunchCommand>();
}

} // namespace commands
} // namespace fairyfly
