#include "include/commands/command_base.h"

namespace fairyfly::commands {

class LoginCommand : public CommandBase {
public:
    std::string name() const override { return "login"; }
    std::string description() const override { return "Log into a launched SAP GUI session"; }

    CLI::App* setup_cli(CLI::App& app) override {
        cmd_ = app.add_subcommand(name(), description());
        cmd_->add_option("--credentials-file", credentials_file_, "Colon-separated credential file (deprecated: use `credentials import-env`)");
        cmd_->add_flag("--credentials-stdin", from_stdin_, "Read the same credential lines from standard input");
        cmd_->add_option("--credential", credential_name_,
                         "Stored credential name (see `credentials set`); default: the session's connection name");
        cmd_->add_option("--connection", connection_id_, "Saved Fairyfly connection ID");
        cmd_->add_option("--multiple-logon", multiple_logon_,
                         "If the user is already logged on: fail (default, leave the dialog open), keep "
                         "(continue without ending other logons), terminate (end this logon, closing the session), "
                         "end (DESTRUCTIVE: ends the user's other logons, unsaved data is lost; refused under --read-only)")
            ->check(CLI::IsMember({"fail", "keep", "end", "terminate"}))
            ->default_str("fail");
        return cmd_;
    }

    Result execute(cli::CommandHandler& handler) override {
        if (!credentials_file_.empty() && from_stdin_) {
            Result result;
            result.status = Result::Status::Error;
            result.error = {{"code", "INVALID_ARGUMENT"},
                            {"message", "Choose at most one of --credentials-file or --credentials-stdin"}};
            return result;
        }
        return handler.handle_login(credentials_file_, connection_id_, from_stdin_, credential_name_, multiple_logon_);
    }

    bool was_invoked() const override { return cmd_ && *cmd_; }

private:
    CLI::App* cmd_ = nullptr;
    std::string credentials_file_;
    std::string credential_name_;
    std::string multiple_logon_ = "fail";
    bool from_stdin_ = false;
    std::optional<int> connection_id_;
};

std::unique_ptr<CommandBase> create_login_command() {
    return std::make_unique<LoginCommand>();
}

} // namespace fairyfly::commands
