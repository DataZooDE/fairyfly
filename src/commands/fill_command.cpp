#include "include/commands/command_base.h"

#include <optional>

namespace fairyfly {
namespace commands {

class FillCommand : public CommandBase {
public:
    std::string name() const override { return "element fill"; }

    std::string description() const override {
        return "Fill text field";
    }

    CLI::App* setup_cli(CLI::App& app) override {
        cmd_ = add_leaf(app, {"element","fill"});
        cmd_->add_option("element", element_, "Element ID")
            ->required();
        value_option_ = cmd_->add_option("value", value_, "Value to enter");
        cmd_->add_flag("--clear", clear_, "Clear the field without a value argument");
        cmd_->add_option("--connection", conn_id_, "Connection ID to use");
        cmd_->add_option("--row", row_, "Zero-based GridView row index");
        cmd_->add_option("--column", column_, "GridView column ID");
        cmd_->add_flag("--checkbox", checkbox_, "Set a GridView checkbox cell");
        cmd_->add_flag("--commit", commit_, "Notify SAP after the final GridView cell change");
        cmd_->add_flag("--allow-fill", allow_fill_, "Permit fill while --read-only / FAIRYFLY_READ_ONLY is active");
        return cmd_;
    }

    Result execute(cli::CommandHandler& handler) override {
        if (clear_ == (value_option_->count() > 0)) {
            Result result;
            result.status = Result::Status::Error;
            result.error = {
                {"code", "INVALID_ARGUMENT"},
                {"message", clear_ ? "Use either a value or --clear" : "A value or --clear is required"}
            };
            return result;
        }
        return handler.handle_fill(element_, value_, conn_id_, row_, column_, checkbox_, commit_, allow_fill_);
    }

    bool was_invoked() const override {
        return cmd_ && *cmd_;
    }

private:
    CLI::App* cmd_ = nullptr;
    CLI::Option* value_option_ = nullptr;
    std::string element_;
    std::string value_;
    bool clear_ = false;
    std::optional<int> conn_id_;
    std::optional<int> row_;
    std::string column_;
    bool checkbox_ = false;
    bool commit_ = false;
    bool allow_fill_ = false;
};

// Factory function
// Factory function
std::unique_ptr<CommandBase> create_fill_command() {
    return std::make_unique<FillCommand>();
}

} // namespace commands
} // namespace fairyfly
