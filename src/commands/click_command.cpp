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
        cmd_->add_option("element", element_, "Element ID (e.g., wnd[0]/usr/btn[3])")
            ->required();
        cmd_->add_option("--connection", conn_id_, "Connection ID to use");
        return cmd_;
    }

    Result execute(cli::CommandHandler& handler) override {
        return handler.handle_click(element_, conn_id_);
    }

    bool was_invoked() const override {
        return cmd_ && *cmd_;
    }

private:
    CLI::App* cmd_ = nullptr;
    std::string element_;
    std::optional<int> conn_id_;
};

// Factory function
// Factory function
std::unique_ptr<CommandBase> create_click_command() {
    return std::make_unique<ClickCommand>();
}

} // namespace commands
} // namespace fairyfly
