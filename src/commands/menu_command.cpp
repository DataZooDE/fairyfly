#include "include/commands/command_base.h"

#include <optional>

namespace fairyfly {
namespace commands {

/// `menu list` lists the menu bar tree, `menu select PATH` selects one item.
class MenuCommand : public CommandBase {
public:
    std::string name() const override { return "menu"; }

    std::string description() const override { return "Menu bar operations"; }

    CLI::App* setup_cli(CLI::App& app) override {
        menu_cmd_ = noun_app(app, "menu");

        list_cmd_ = add_leaf(app, {"menu", "list"});
        list_cmd_->add_option("--window", list_window_, "Window whose menu bar is listed (default wnd[0], or @active)")
            ->default_val("wnd[0]");
        list_cmd_->add_option("--connection", list_conn_id_, "Connection ID to use");

        select_cmd_ = add_leaf(app, {"menu", "select"});
        select_cmd_->add_option("path", select_path_,
            "Menu text path to select, e.g. 'Runtime Errors/Display' (case-insensitive, '&' ignored)")
            ->required();
        select_cmd_->add_option("--window", select_window_, "Window whose menu bar is used (default wnd[0], or @active)")
            ->default_val("wnd[0]");
        select_cmd_->add_option("--connection", select_conn_id_, "Connection ID to use");
        return menu_cmd_;
    }

    Result execute(cli::CommandHandler& handler) override {
        if (*list_cmd_) return handler.handle_screen_menu("", list_window_, list_conn_id_);
        if (*select_cmd_) {
            if (select_path_.empty()) {
                Result result;
                result.status = Result::Status::Error;
                result.error = {{"code", "INVALID_ARGUMENT"}, {"message", "menu select needs a non-empty menu path"}};
                return result;
            }
            return handler.handle_screen_menu(select_path_, select_window_, select_conn_id_);
        }
        Result result;
        result.status = Result::Status::Error;
        result.error["code"] = "NO_SUBCOMMAND";
        result.error["message"] = "No menu subcommand specified (list or select)";
        return result;
    }

    bool was_invoked() const override { return menu_cmd_ && *menu_cmd_; }

private:
    CLI::App* menu_cmd_ = nullptr;
    CLI::App* list_cmd_ = nullptr;
    CLI::App* select_cmd_ = nullptr;
    std::string list_window_ = "wnd[0]";
    std::optional<int> list_conn_id_;
    std::string select_path_;
    std::string select_window_ = "wnd[0]";
    std::optional<int> select_conn_id_;
};

std::unique_ptr<CommandBase> create_menu_command() {
    return std::make_unique<MenuCommand>();
}

} // namespace commands
} // namespace fairyfly
