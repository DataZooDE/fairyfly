#include "include/commands/command_base.h"

#include <optional>

namespace fairyfly {
namespace commands {

class PressF4Command : public CommandBase {
public:

    CLI::App* setup_cli(CLI::App& app) override {
        cmd_ = add_leaf(app, {"element","f4"});
        cmd_->footer(R"HELP(Usage notes:
Open search help on a field found by screen read --only-f4-fields, then read the popup.
Example: fairyfly element f4 "wnd[0]/usr/ctxtFIELD" --connection 3
)HELP");
        cmd_->add_option("element", element_, "Element ID of GuiCTextField (e.g., wnd[0]/usr/ctxtFIELD or @active/usr/ctxtFIELD)")
            ->required();
        cmd_->add_option("--connection", conn_id_, "Connection ID to use");
        return cmd_;
    }

    Result execute(cli::CommandHandler& handler) override {
        return handler.handle_press_f4(element_, conn_id_);
    }

private:
    std::string element_;
    std::optional<int> conn_id_;
};

// Factory function
std::unique_ptr<CommandBase> create_press_f4_command() {
    return std::make_unique<PressF4Command>();
}

} // namespace commands
} // namespace fairyfly
