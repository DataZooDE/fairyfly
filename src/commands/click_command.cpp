#include "include/commands/command_base.h"

#include <optional>

namespace fairyfly {
namespace commands {

class ClickCommand : public CommandBase {
public:
    std::string name() const override { return "element click"; }

    std::string description() const override {
        return "Click UI element";
    }

    CLI::App* setup_cli(CLI::App& app) override {
        cmd_ = add_leaf(app, {"element","click"});
        cmd_->add_option("element", element_, "Element ID (e.g., wnd[0]/usr/btn[3] or @active/usr/btn[3])")
            ->required();
        cmd_->add_option("--connection", conn_id_, "Connection ID to use");
        cmd_->add_flag("--wait-for-window", wait_for_window_,
            "Wait for new window to open after click (e.g., popup/dialog). Detects only a changed window, title, "
            "transaction or status bar text; a click that only changes field/grid contents is reported "
            "as unchanged after the timeout");
        cmd_->add_option("--timeout", timeout_ms_,
            "Timeout in milliseconds for window detection (default: 5000)")
            ->default_val(5000);
        cmd_->add_option("--node-key", node_key_,
            "Tree node key (required for tree elements, see 'element get --list-nodes')");
        cmd_->add_option("--tree-action", tree_action_,
            "Tree action: select, expand, collapse, doubleclick, contextmenu (default: doubleclick)")
            ->default_val("doubleclick")
            ->check(CLI::IsMember({"select", "expand", "collapse", "doubleclick", "contextmenu"}));
        cmd_->add_option("--menu-item", menu_item_,
            "Context-menu item text (required with --tree-action contextmenu)");
        cmd_->add_option("--row", row_, "Zero-based GridView row to select");
        cmd_->add_option("--column", column_, "GridView column ID to activate");
        cmd_->add_flag("--doubleclick", doubleclick_,
            "Double-click the GridView cell at --row/--column (opens the row's detail)");
        return cmd_;
    }

    Result execute(cli::CommandHandler& handler) override {
        return handler.handle_click(element_, conn_id_, wait_for_window_, timeout_ms_,
                                     node_key_, tree_action_, menu_item_, row_, column_,
                                     doubleclick_);
    }

    bool was_invoked() const override {
        return cmd_ && *cmd_;
    }

private:
    CLI::App* cmd_ = nullptr;
    std::string element_;
    std::optional<int> conn_id_;
    bool wait_for_window_ = false;
    int timeout_ms_ = 5000;
    std::string node_key_;           // Tree node key
    std::string tree_action_ = "doubleclick";  // Tree action
    std::string menu_item_;
    std::optional<int> row_;
    std::string column_;
    bool doubleclick_ = false;
};

// Factory function
// Factory function
std::unique_ptr<CommandBase> create_click_command() {
    return std::make_unique<ClickCommand>();
}

} // namespace commands
} // namespace fairyfly
