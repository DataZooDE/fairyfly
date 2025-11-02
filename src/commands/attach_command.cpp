#include "include/commands/command_base.h"

namespace fairyfly {
namespace commands {

class AttachCommand : public CommandBase {
public:
    std::string name() const override { return "attach"; }

    std::string description() const override {
        return "Attach to running SAP GUI window";
    }

    CLI::App* setup_cli(CLI::App& app) override {
        cmd_ = app.add_subcommand(name(), description());
        cmd_->add_option("--timeout", timeout_, "Timeout in seconds for window selection")
            ->check(CLI::PositiveNumber);
        return cmd_;
    }

    Result execute(cli::CommandHandler& handler) override {
        return handler.handle_attach(timeout_);
    }

    bool was_invoked() const override {
        return cmd_ && *cmd_;
    }

private:
    CLI::App* cmd_ = nullptr;
    int timeout_ = 20;
};

// Factory function
std::unique_ptr<CommandBase> create_attach_command() {
    return std::make_unique<AttachCommand>();
}

} // namespace commands
} // namespace fairyfly
