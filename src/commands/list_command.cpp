#include "include/commands/command_base.h"


namespace fairyfly {
namespace commands {

class ListCommand : public CommandBase {
public:
    std::string name() const override { return "list"; }

    std::string description() const override {
        return "List all SAP GUI connections, sessions, and windows";
    }

    CLI::App* setup_cli(CLI::App& app) override {
        cmd_ = app.add_subcommand(name(), description());
        return cmd_;
    }

    Result execute(cli::CommandHandler& handler) override {
        return handler.handle_list_all();
    }

    bool was_invoked() const override {
        return cmd_ && *cmd_;
    }

private:
    CLI::App* cmd_ = nullptr;
};

// Factory function
// Factory function
std::unique_ptr<CommandBase> create_list_command() {
    return std::make_unique<ListCommand>();
}

} // namespace commands
} // namespace fairyfly
