#include "include/commands/command_base.h"


namespace fairyfly {
namespace commands {

class ListCommand : public CommandBase {
public:
    std::string name() const override { return "session list"; }

    std::string description() const override {
        return "List all SAP GUI connections, sessions, and windows";
    }

    CLI::App* setup_cli(CLI::App& app) override {
        cmd_ = add_leaf(app, {"session","list"});
        cmd_->footer(R"HELP(Usage notes:
Lists live SAP GUI sessions. Copy sessions[].id into session attach --session-id.
The indices here are SAP enumeration indices, not --connection values.
Example: fairyfly session list
)HELP");
        add_output_option(cmd_, output_format_);
        return cmd_;
    }

    Result execute(cli::CommandHandler& handler) override {
        return handler.handle_list_all();
    }

    bool was_invoked() const override {
        return cmd_ && *cmd_;
    }

    std::optional<std::string> get_preferred_output_format() const override {
        return output_override(output_format_);
    }

private:
    CLI::App* cmd_ = nullptr;
    std::string output_format_;
};

// Factory function
// Factory function
std::unique_ptr<CommandBase> create_list_command() {
    return std::make_unique<ListCommand>();
}

} // namespace commands
} // namespace fairyfly
