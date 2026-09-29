#include "include/commands/command_base.h"

#include <optional>

namespace fairyfly {
namespace commands {

class SendKeyCommand : public CommandBase {
public:
    std::string name() const override { return "send-key"; }

    std::string description() const override {
        return "Send a key (enter, f1..f12, shift+f1..shift+f12 or a raw VKey number) to a window";
    }

    CLI::App* setup_cli(CLI::App& app) override {
        cmd_ = app.add_subcommand(name(), description());
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

    bool was_invoked() const override {
        return cmd_ && *cmd_;
    }

private:
    CLI::App* cmd_ = nullptr;
    std::string key_;
    std::string window_ = "@active";
    std::optional<int> conn_id_;
};

std::unique_ptr<CommandBase> create_send_key_command() {
    return std::make_unique<SendKeyCommand>();
}

} // namespace commands
} // namespace fairyfly
