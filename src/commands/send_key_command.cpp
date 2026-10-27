#include "include/commands/command_base.h"

#include <optional>

namespace fairyfly {
namespace commands {

class SendKeyCommand : public CommandBase {
public:

    CLI::App* setup_cli(CLI::App& app) override {
        cmd_ = add_leaf(app, {"key","send"});
        cmd_->footer(R"HELP(Usage notes:
Sends a SAP virtual key to the active window by default. Enter/F8 can execute reports.
Read the resulting screen before continuing.
Example: fairyfly key send f3 --connection 3
)HELP");
        cmd_->add_option("key", key_, "Key name (enter, f3, f8, shift+f4, ...) or raw SAP VKey number")
            ->required();
        cmd_->add_option("--window", window_, "Target window: @active (default) or wnd[N]")
            ->default_val("@active");
        cmd_->add_option("--connection", conn_id_, "Connection ID to use");
        return cmd_;
    }

    Result execute(cli::CommandHandler& handler) override {
        return handler.handle_send_key(key_, window_, conn_id_);
    }

private:
    std::string key_;
    std::string window_ = "@active";
    std::optional<int> conn_id_;
};

std::unique_ptr<CommandBase> create_send_key_command() {
    return std::make_unique<SendKeyCommand>();
}

} // namespace commands
} // namespace fairyfly
