#include "include/commands/command_base.h"

#include <fstream>
#include <optional>
#include <sstream>

#include <nlohmann/json.hpp>

#include "include/fill_fields.h"

namespace fairyfly {
namespace commands {

class FillCommand : public CommandBase {
public:

    CLI::App* setup_cli(CLI::App& app) override {
        cmd_ = add_leaf(app, {"element","fill"});
        cmd_->footer(R"HELP(Usage notes:
Use a value or --clear. Read the result's value to see SAP normalization.
For a GridView cell supply both --row and --column; --commit notifies SAP of the final edit.
Several fields of one screen in one call: --fields with a JSON array (or @file) of
{"element","value"} / {"element","clear":true} / {"element","row","column","value"} objects,
optionally followed by --then-key enter|f8 (pressed on the main window only after every field succeeded).
Examples:
  fairyfly element fill "wnd[0]/usr/ctxtFIELD" "VALUE" --connection 3
  fairyfly element fill "wnd[0]/usr/ctxtFIELD" --clear --connection 3
  fairyfly element fill --fields '[{"element":"wnd[0]/usr/ctxtP_A","value":"1"},{"element":"wnd[0]/usr/ctxtP_B","value":"2"}]' --then-key f8
)HELP");
        element_option_ = cmd_->add_option("element", element_, "Element ID (not with --fields)");
        value_option_ = cmd_->add_option("value", value_, "Value to enter. Check box: true/false/1/0/yes/no/on/off/x (sets Selected); "
            "radio button: true/1/x selects it (it cannot be cleared); combo box: an entry key or its displayed value");
        cmd_->add_flag("--clear", clear_, "Clear the field without a value argument");
        cmd_->add_option("--connection", conn_id_, "Connection ID to use");
        cmd_->add_option("--row", row_, "Zero-based GridView row index");
        cmd_->add_option("--column", column_, "GridView column ID");
        cmd_->add_flag("--checkbox", checkbox_, "Set a GridView checkbox cell");
        cmd_->add_flag("--commit", commit_, "Notify SAP after the final GridView cell change");
        fields_option_ = cmd_->add_option("--fields", fields_, "Several fields at once: a JSON array (or @file) of "
            "{element, value | clear, row, column, checkbox, commit} objects, at most 50");
        cmd_->add_option("--then-key", then_key_, "With --fields: press enter or f8 on the main window after every field succeeded");
        cmd_->add_flag("--keep-going", keep_going_, "With --fields: fill the remaining fields after a failure (the key is still not pressed)");
        cmd_->add_flag("--allow-fill", allow_fill_, "Permit fill while --read-only / FAIRYFLY_READ_ONLY is active");
        return cmd_;
    }

    Result execute(cli::CommandHandler& handler) override {
        if (fields_option_->count() > 0) return execute_fields(handler);
        if (!then_key_.empty() || keep_going_) return invalid("--then-key and --keep-going need --fields");
        if (element_option_->count() == 0) return invalid("an element id (or --fields) is required");
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

    static Result invalid(const std::string& message) {
        Result result;
        result.status = Result::Status::Error;
        result.error = {{"code", "INVALID_ARGUMENT"}, {"message", message}};
        return result;
    }

    Result execute_fields(cli::CommandHandler& handler) {
        if (element_option_->count() > 0 || value_option_->count() > 0 || clear_ || row_ || !column_.empty() || checkbox_ ||
            commit_)
            return invalid("--fields replaces the element, value, --clear, --row, --column, --checkbox and --commit arguments");
        std::string text = fields_;
        if (!text.empty() && text[0] == '@') {
            std::ifstream in(text.substr(1), std::ios::binary);
            if (!in) return invalid("cannot read the --fields file " + text.substr(1));
            std::ostringstream buf;
            buf << in.rdbuf();
            text = buf.str();
        }
        const auto parsed = nlohmann::json::parse(text, nullptr, false);
        if (parsed.is_discarded()) return invalid("--fields is not valid JSON");
        std::string why;
        const auto fields = sap::parse_fill_fields(parsed, why);
        if (!fields) return invalid(why);
        return handler.handle_fill_fields(*fields, then_key_, !keep_going_, conn_id_, allow_fill_);
    }

private:
    CLI::Option* value_option_ = nullptr;
    CLI::Option* element_option_ = nullptr;
    CLI::Option* fields_option_ = nullptr;
    std::string fields_;
    std::string then_key_;
    bool keep_going_ = false;
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
