#include "include/commands/command_base.h"

#include <optional>

namespace fairyfly {
namespace commands {

class ClickCommand : public CommandBase {
public:
    std::string name() const override { return "click"; }

    std::string description() const override {
        return "Click UI element";
    }

    CLI::App* setup_cli(CLI::App& app) override {
        cmd_ = app.add_subcommand(name(), description());
        cmd_->add_option("element", element_, "Element ID (e.g., wnd[0]/usr/btn[3] or @active/usr/btn[3])")
            ->required();
        cmd_->add_option("--connection", conn_id_, "Connection ID to use");
        cmd_->add_flag("--wait-for-window", wait_for_window_,
            "Wait for new window to open after click (e.g., popup/dialog)");
        cmd_->add_option("--timeout", timeout_ms_,
            "Timeout in milliseconds for window detection (default: 5000)")
            ->default_val(5000);
        return cmd_;
    }

    Result execute(cli::CommandHandler& handler) override {
        return handler.handle_click(element_, conn_id_, wait_for_window_, timeout_ms_);
    }

    bool was_invoked() const override {
        return cmd_ && *cmd_;
    }

private:
    CLI::App* cmd_ = nullptr;
    std::string element_;
    std::optional<int> conn_id_;
    bool wait_for_window_ = false;
    int timeout_ms_ = 5000;
};

// Factory function
// Factory function
std::unique_ptr<CommandBase> create_click_command() {
    return std::make_unique<ClickCommand>();
}

} // namespace commands
} // namespace fairyfly
