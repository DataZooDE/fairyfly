#include "include/commands/command_base.h"

namespace fairyfly {
namespace commands {

class AttachCommand : public CommandBase {
public:
    std::string name() const override { return "session attach"; }

    std::string description() const override {
        return "Attach to a running SAP GUI window or exact session ID";
    }

    CLI::App* setup_cli(CLI::App& app) override {
        cmd_ = add_leaf(app, {"session","attach"});
        cmd_->add_option("--timeout", timeout_, "Timeout in seconds for window selection")
            ->check(CLI::PositiveNumber);
        session_option_ = cmd_->add_option("--session-id", session_id_, "Exact SAP GUI session ID from list");
        return cmd_;
    }

    Result execute(cli::CommandHandler& handler) override {
        return handler.handle_attach(timeout_, session_option_->count() ? std::optional<std::string>(session_id_) : std::nullopt);
    }

    bool was_invoked() const override {
        return cmd_ && *cmd_;
    }

private:
    CLI::App* cmd_ = nullptr;
    int timeout_ = 20;
    std::string session_id_;
    CLI::Option* session_option_ = nullptr;
};

// Factory function
std::unique_ptr<CommandBase> create_attach_command() {
    return std::make_unique<AttachCommand>();
}

} // namespace commands
} // namespace fairyfly
