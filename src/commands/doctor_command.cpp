#include "include/commands/command_base.h"

namespace fairyfly {
namespace commands {

class DoctorCommand : public CommandBase {
public:
    std::string name() const override { return "doctor"; }

    std::string description() const override {
        return "Run comprehensive preflight environment and scripting diagnostics";
    }

    CLI::App* setup_cli(CLI::App& app) override {
        cmd_ = add_leaf(app, {"doctor"});
        return cmd_;
    }

    Result execute(cli::CommandHandler& handler) override {
        return handler.handle_doctor();
    }

    bool was_invoked() const override {
        return cmd_ && *cmd_;
    }

private:
    CLI::App* cmd_ = nullptr;
};

// Factory function
std::unique_ptr<CommandBase> create_doctor_command() {
    return std::make_unique<DoctorCommand>();
}

} // namespace commands
} // namespace fairyfly
