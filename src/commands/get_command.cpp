#include "include/commands/command_base.h"

#include <optional>

namespace fairyfly {
namespace commands {

class GetCommand : public CommandBase {
public:
    std::string name() const override { return "get"; }

    std::string description() const override {
        return "Read field value";
    }

    CLI::App* setup_cli(CLI::App& app) override {
        cmd_ = app.add_subcommand(name(), description());
        cmd_->add_option("element", element_, "Element ID")
            ->required();
        cmd_->add_option("--connection", conn_id_, "Connection ID to use");
        cmd_->add_flag("--list-nodes", list_nodes_,
            "List all nodes in tree (only for tree elements)");
        return cmd_;
    }

    Result execute(cli::CommandHandler& handler) override {
        return handler.handle_read_field(element_, conn_id_, list_nodes_);
    }

    bool was_invoked() const override {
        return cmd_ && *cmd_;
    }

private:
    CLI::App* cmd_ = nullptr;
    std::string element_;
    std::optional<int> conn_id_;
    bool list_nodes_ = false;  // Tree node listing flag
};

// Factory function
// Factory function
std::unique_ptr<CommandBase> create_get_command() {
    return std::make_unique<GetCommand>();
}

} // namespace commands
} // namespace fairyfly
