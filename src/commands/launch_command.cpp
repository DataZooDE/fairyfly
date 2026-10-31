#include "include/commands/command_base.h"
#include "include/startup_settings.h"
#include "include/tray/tray_win32.h"

namespace fairyfly {
namespace commands {

class LaunchCommand : public CommandBase {
public:

    CLI::App* setup_cli(CLI::App& app) override {
        cmd_ = add_leaf(app, {"session","launch"});
        cmd_->footer(R"HELP(Usage notes:
Reuse an existing logged-in session when available. Launch alone opens SAP Logon;
use --login to authenticate with stored credentials, or call session login afterward.
Keep the returned connection_id. Check login succeeded before starting a transaction.
Example: fairyfly session launch "DEV" --login
)HELP");
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
            return Result::fail("INVALID_ARGUMENT", "--credential requires --login");
        }
        if (!login_ && !multiple_logon_.empty()) {
            return Result::fail("INVALID_ARGUMENT", "--multiple-logon requires --login");
        }
        return handler.handle_launch(connection_name_, allow_sapshcut_, login_, credential_name_,
                                     multiple_logon_.empty() ? "fail" : multiple_logon_);
    }

private:
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

class SessionDefaultCommand : public CommandBase {
public:
    CLI::App* setup_cli(CLI::App& app) override {
        cmd_ = add_leaf(app, {"session", "default"});
        cmd_->add_option("connection", connection_, "Local SAP Logon entry name; omit to show the current default");
        cmd_->footer(R"HELP(Usage notes:
  Stores the default SAP Logon entry locally for on-demand startup in CLI, MCP and link mode.
  Saving a default does not enable startup. Tray modes use Allow automatic SAP startup;
  headless commands require --allow-sap-startup. Link also requires the gateway administrator's permission.
Examples:
  fairyfly session default
  fairyfly session default "Bigfox"
  fairyfly --allow-sap-startup --read-only screen read
)HELP");
        return cmd_;
    }
    Result execute(cli::CommandHandler&) override {
        auto settings = tray::make_win32_tray_settings();
        if (cmd_->get_option("connection")->count()) return startup::configure_default(*settings, connection_);
        return Result::ok({{"default_connection", settings->get(tray::kSapDefaultConnectionSetting).value_or("")},
                           {"tray_startup_enabled", startup::read_local(*settings, true, false).local_enabled}});
    }
private:
    std::string connection_;
};

std::unique_ptr<CommandBase> create_session_default_command() {
    return std::make_unique<SessionDefaultCommand>();
}

} // namespace commands
} // namespace fairyfly
