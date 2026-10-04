#include "include/commands/command_base.h"
#include "include/cli_handler.h"

// NOTE: there is intentionally no --password option anywhere: a secret on the command line
// ends up in shell history and process listings. Secrets come from the console prompt or stdin.
namespace fairyfly {
namespace commands {

class CredentialsCommand : public CommandBase {
public:
    std::string name() const override { return "credentials"; }

    std::string description() const override {
        return "Manage SAP logon credentials in the Windows Credential Manager";
    }

    CLI::App* setup_cli(CLI::App& app) override {
        cmd_ = noun_app(app, "credentials");

        set_cmd_ = add_leaf(app, {"credentials", "set"});
        set_cmd_->footer(R"HELP(Usage notes:
Stores credentials in Windows Credential Manager. Prompts for the password unless
--password-stdin is provided. Requires console/stdin; unavailable inside batch.
Example: fairyfly credentials set DEV --user SAPUSER --client 001
)HELP");
        set_cmd_->add_option("connection", set_connection_, "Connection name, e.g. Bigfox")->required();
        set_cmd_->add_option("--user", set_user_, "SAP user name")->required();
        set_cmd_->add_option("--client", set_client_, "Three-digit SAP client, e.g. 001")->required();
        set_cmd_->add_option("--language", set_language_, "Logon language (default EN)");
        set_cmd_->add_flag("--password-stdin", password_stdin_, "Read the password from the first line of standard input");

        list_cmd_ = add_leaf(app, {"credentials", "list"});
        list_cmd_->footer(R"HELP(Usage notes:
Lists stored credential names without passwords.
Example: fairyfly credentials list
)HELP");
        add_output_option(list_cmd_, list_output_);

        delete_cmd_ = add_leaf(app, {"credentials", "delete"});
        delete_cmd_->footer(R"HELP(Usage notes:
Deletes the named stored credential.
Example: fairyfly credentials delete DEV
)HELP");
        delete_cmd_->add_option("connection", delete_connection_, "Connection name")->required();

        import_cmd_ = add_leaf(app, {"credentials", "import-env"});
        import_cmd_->footer(R"HELP(Usage notes:
Imports a colon-separated credential file into Windows Credential Manager.
--delete-file removes the source only after a successful import.
Example: fairyfly credentials import-env credentials.txt --connection DEV --delete-file
)HELP");
        import_cmd_->add_option("path", import_path_, "Colon-separated credential file to import")->required();
        import_cmd_->add_option("--connection", import_connection_, "Connection name when the file has no Connection: line");
        import_cmd_->add_flag("--delete-file", delete_file_, "Delete the file after a successful import");
        return cmd_;
    }

    Result execute(cli::CommandHandler& handler) override {
        if (*set_cmd_) {
            return handler.handle_credentials_set(set_connection_, set_user_, set_client_, set_language_, password_stdin_);
        }
        if (*list_cmd_) return handler.handle_credentials_list();
        if (*delete_cmd_) return handler.handle_credentials_delete(delete_connection_);
        if (*import_cmd_) {
            return handler.handle_credentials_import_env(import_path_,
                                                         import_connection_, delete_file_);
        }
        Result result;
        result.status = Result::Status::Error;
        result.error["code"] = "NO_SUBCOMMAND";
        result.error["message"] = "No credentials subcommand specified (set, list, delete, or import-env)";
        return result;
    }

    bool was_invoked() const override { return cmd_ && *cmd_; }

    std::optional<std::string> get_preferred_output_format() const override {
        if (list_cmd_ && *list_cmd_) return output_override(list_output_);
        return std::nullopt;
    }

private:
    CLI::App* cmd_ = nullptr;
    CLI::App* set_cmd_ = nullptr;
    CLI::App* list_cmd_ = nullptr;
    CLI::App* delete_cmd_ = nullptr;
    CLI::App* import_cmd_ = nullptr;

    std::string set_connection_;
    std::string set_user_;
    std::string set_client_;
    std::string set_language_;
    bool password_stdin_ = false;
    std::string list_output_;
    std::string delete_connection_;
    std::string import_path_;
    std::string import_connection_;
    bool delete_file_ = false;
};

std::unique_ptr<CommandBase> create_credentials_command() {
    return std::make_unique<CredentialsCommand>();
}

} // namespace commands
} // namespace fairyfly
