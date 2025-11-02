#include "include/commands/command_base.h"

#include <optional>

namespace fairyfly {
namespace commands {

class FillCommand : public CommandBase {
public:
    std::string name() const override { return "fill"; }

    std::string description() const override {
        return "Fill text field";
    }

    CLI::App* setup_cli(CLI::App& app) override {
        cmd_ = app.add_subcommand(name(), description());
        cmd_->add_option("element", element_, "Element ID")
            ->required();
        cmd_->add_option("value", value_, "Value to enter")
            ->required();
        cmd_->add_option("--connection", conn_id_, "Connection ID to use");
        return cmd_;
    }

    Result execute(cli::CommandHandler& handler) override {
        return handler.handle_fill(element_, value_, conn_id_);
    }

    bool was_invoked() const override {
        return cmd_ && *cmd_;
    }

private:
    CLI::App* cmd_ = nullptr;
    std::string element_;
    std::string value_;
    std::optional<int> conn_id_;
};

// Factory function
// Factory function
std::unique_ptr<CommandBase> create_fill_command() {
    return std::make_unique<FillCommand>();
}

} // namespace commands
} // namespace fairyfly
