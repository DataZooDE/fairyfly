#include "include/commands/command_base.h"

#include <optional>

namespace fairyfly {
namespace commands {

class TcodeCommand : public CommandBase {
public:
    std::string name() const override { return "transaction start"; }

    std::string description() const override {
        return "Execute SAP transaction";
    }

    CLI::App* setup_cli(CLI::App& app) override {
        cmd_ = add_leaf(app, {"transaction","start"});
        cmd_->add_option("code", tcode_, "Transaction code (e.g., SE38, VA01)")
            ->required();
        cmd_->add_option("--connection", conn_id_, "Connection ID to use");
        return cmd_;
    }

    Result execute(cli::CommandHandler& handler) override {
        return handler.handle_transaction(tcode_, conn_id_);
    }

    bool was_invoked() const override {
        return cmd_ && *cmd_;
    }

private:
    CLI::App* cmd_ = nullptr;
    std::string tcode_;
    std::optional<int> conn_id_;
};

// Factory function
// Factory function
std::unique_ptr<CommandBase> create_tcode_command() {
    return std::make_unique<TcodeCommand>();
}

} // namespace commands
} // namespace fairyfly
