#include "include/commands/command_base.h"

#include <optional>

namespace fairyfly {
namespace commands {

class GetCommand : public CommandBase {
public:
    std::string name() const override { return "element get"; }

    std::string description() const override {
        return "Read field value";
    }

    CLI::App* setup_cli(CLI::App& app) override {
        cmd_ = add_leaf(app, {"element","get"});
        cmd_->footer(R"HELP(Usage notes:
Read a control identified by screen find or screen read.
For trees, --list-nodes returns data.nodes with key and text. Collapsed parents may
have unloaded children: this is not a complete backend inventory. Expand a returned
parent with element click --tree-action expand --node-key, then list again.
Quote node keys exactly, including leading spaces. TREE below is the observed tree ID.
For an inactive tab, --activate-tab reads the field and restores the prior tab.
Examples:
  fairyfly element get "wnd[0]/usr/ctxtFIELD" --connection 3
  fairyfly element get "TREE" --list-nodes --connection 3
  fairyfly element click "TREE" --tree-action expand --node-key "          1" --connection 3
  fairyfly element get "TREE" --list-nodes --connection 3
)HELP");
        cmd_->add_option("element", element_, "Element ID")
            ->required();
        cmd_->add_option("--connection", conn_id_, "Connection ID to use");
        cmd_->add_flag("--list-nodes", list_nodes_,
            "List all nodes in tree (only for tree elements)");
        cmd_->add_flag("--activate-tab", activate_tab_,
            "If the element is on an inactive tab page, select that tab, read the element and restore the previous tab "
            "(without it: error ELEMENT_ON_INACTIVE_TAB)");
        add_output_option(cmd_, output_format_);
        return cmd_;
    }

    Result execute(cli::CommandHandler& handler) override {
        return handler.handle_read_field(element_, conn_id_, list_nodes_, activate_tab_);
    }

    bool was_invoked() const override {
        return cmd_ && *cmd_;
    }

    std::optional<std::string> get_preferred_output_format() const override {
        return output_override(output_format_);
    }

private:
    CLI::App* cmd_ = nullptr;
    std::string element_;
    std::optional<int> conn_id_;
    bool list_nodes_ = false;  // Tree node listing flag
    bool activate_tab_ = false;
    std::string output_format_;
};

// Factory function
// Factory function
std::unique_ptr<CommandBase> create_get_command() {
    return std::make_unique<GetCommand>();
}

} // namespace commands
} // namespace fairyfly
