#include "include/commands/command_base.h"

namespace fairyfly {
namespace commands {

class DoctorCommand : public CommandBase {
public:

    CLI::App* setup_cli(CLI::App& app) override {
        cmd_ = add_leaf(app, {"doctor"});
        cmd_->footer(R"HELP(Usage notes:
Use when SAP GUI attachment or scripting fails; inspect reported prerequisites.
Example: fairyfly doctor
)HELP");
        return cmd_;
    }

    Result execute(cli::CommandHandler& handler) override {
        return handler.handle_doctor();
    }

private:
};

// Factory function
std::unique_ptr<CommandBase> create_doctor_command() {
    return std::make_unique<DoctorCommand>();
}

} // namespace commands
} // namespace fairyfly
