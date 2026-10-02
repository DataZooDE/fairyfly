#include "include/commands/command_base.h"
#include "include/cli_handler.h"
#include "include/constants.h"
#include "include/screen_reader.h"
#include <spdlog/spdlog.h>
#include <optional>

namespace fairyfly {
namespace commands {

class ScreenCommand : public CommandBase {
public:
    std::string name() const override { return "screen"; }

    std::string description() const override {
        return "Screen operations";
    }

    CLI::App* setup_cli(CLI::App& app) override {
        // Create parent "screen" subcommand once
        screen_cmd_ = noun_app(app, "screen");

        // Add "read" subcommand
        read_cmd_ = add_leaf(app, {"screen", "read"});
        read_cmd_->add_flag_callback("--no-children", [this]() { read_children_ = false; }, "Don't include child elements");
        read_cmd_->add_flag("--no-tabs", no_tabs_, "Skip tab expansion (faster, less complete)");
        read_cmd_->add_option("--tab", only_tab_, "Expand only this tab (tab ID or its trailing part, e.g. tabpTAB2)")
            ->excludes("--no-tabs");
        read_cmd_->add_flag("--skip-trees", skip_trees_, "Skip tree extraction (workaround for problematic trees)");
        read_cmd_->add_flag("--probe-all", probe_all_,
            "Exhaustively probe every container by FindById (slower; restores pre-gating behavior; also FAIRYFLY_PROBE_ALL=1)");
        read_cmd_->add_flag("--compact", compact_, "Compact output. markdown: hide IDs, collapse empty fields. json/toon: hierarchy and per-tab elements become id arrays, empty/null/false element fields omitted");
        read_cmd_->add_option("--max-rows", max_rows_, "Maximum grid/table rows to read (default 20, maximum 200)")
            ->check(CLI::Range(1, constants::MAX_REQUESTED_TABLE_ROWS));
        read_cmd_->add_option("--offset", row_offset_,
            "Index of the first grid/table row to return (default 0); the output reports next_offset when more rows follow")
            ->check(CLI::Range(0, 1000000));
        read_cmd_->add_option("--connection", read_conn_id_, "Connection ID to use");
        read_cmd_->add_option("--output", read_output_format_, "Output format: json, markdown, toon")
            ->check(CLI::IsMember({"json", "markdown", "toon"}));

        // Filter options
        read_cmd_->add_flag("--only-buttons", filters_.only_buttons, "Show only GuiButton elements");
        read_cmd_->add_flag("--only-fields", filters_.only_fields, "Show only input fields");
        read_cmd_->add_flag("--only-editable", filters_.only_editable, "Show only changeable fields");
        read_cmd_->add_flag("--only-f4-fields", filters_.only_f4_fields, "Show only fields with F4 search help");
        read_cmd_->add_flag("--only-tables", filters_.only_tables, "Show only grids and table controls (with their rows)");
        read_cmd_->add_option("--text-contains", filter_text_contains_, "Filter by text/tooltip containing string (case-insensitive); grids and table controls keep only the rows with a matching cell");
        read_cmd_->add_option("--id-contains", filter_id_contains_, "Filter by element ID containing string");
        read_cmd_->add_option("--type", filter_type_, "Filter by exact element type");
        read_cmd_->add_flag("--first", filters_.first_match_only, "Return only first matching element");

        find_cmd_ = add_leaf(app, {"screen", "find"});
        find_cmd_->add_option("--id-contains", find_id_contains_, "Element ID substring (case-sensitive)");
        find_cmd_->add_option("--name-contains", find_name_contains_, "Control name substring (ASCII case-insensitive)");
        find_cmd_->add_option("--type", find_type_, "Exact SAP GUI control type");
        find_cmd_->add_option("--limit", find_limit_, "Maximum matches (default 1, maximum 100)")
            ->check(CLI::Range(1, 100));
        find_cmd_->add_flag("--probe-all", find_probe_all_,
            "Exhaustively probe every container by FindById (slower; also FAIRYFLY_PROBE_ALL=1)");
        find_cmd_->add_option("--connection", find_conn_id_, "Connection ID to use");
        find_cmd_->add_option("--output", find_output_format_, "Output format: json, markdown, toon")
            ->check(CLI::IsMember({"json", "markdown", "toon"}));

        // Add "capture" subcommand
        capture_cmd_ = add_leaf(app, {"screen", "capture"});
        capture_cmd_->add_option("--file,-f", screenshot_file_, "Output file path or '-' for stdout");
        capture_cmd_->add_option("--format", screenshot_format_, "Output format: png, base64")
            ->check(CLI::IsMember({"png", "base64"}));
        capture_cmd_->add_option("--scale", screenshot_scale_,
            "Scale factor (0.0-1.0) or width in pixels, applied AFTER the crop to the cropped image");
        capture_cmd_->add_option("--x", screenshot_x_, "Crop X in NATIVE window pixels (never scaled; applied before --scale)");
        capture_cmd_->add_option("--y", screenshot_y_, "Crop Y in NATIVE window pixels (never scaled; applied before --scale)");
        capture_cmd_->add_option("--width", screenshot_width_, "Crop width in NATIVE window pixels (all four crop options together)");
        capture_cmd_->add_option("--height", screenshot_height_, "Crop height in NATIVE window pixels (all four crop options together). "
            "A crop completely outside the window is INVALID_ARGUMENT naming the native size; the result reports native_size, crop and output_size");
        capture_cmd_->add_flag("--show", screenshot_show_, "Display screenshot in window after capture");
        capture_cmd_->add_option("--connection", capture_conn_id_, "Connection ID to use");

        return screen_cmd_;
    }

    Result execute(cli::CommandHandler& handler) override {
        if (*read_cmd_) {
            bool should_expand_tabs = !no_tabs_ || !only_tab_.empty();

            // Populate optional filter strings from CLI options
            if (!filter_text_contains_.empty()) {
                filters_.text_contains = filter_text_contains_;
            }
            if (!filter_id_contains_.empty()) {
                filters_.id_contains = filter_id_contains_;
            }
            if (!filter_type_.empty()) {
                filters_.type_filter = filter_type_;
            }

            return handler.handle_screen_read(read_children_, read_conn_id_, should_expand_tabs,
                                              filters_, skip_trees_, compact_, max_rows_, only_tab_, probe_all_, row_offset_);
        }
        else if (*find_cmd_) {
            sap::ScreenFindOptions query;
            query.id_contains = find_id_contains_;
            query.name_contains = find_name_contains_;
            query.type = find_type_;
            query.limit = find_limit_;
            query.probe_all = find_probe_all_;
            return handler.handle_screen_find(query, find_conn_id_);
        }
        else if (*capture_cmd_) {
            cli::ScreenshotOptions opts;
            opts.output_file = screenshot_file_;
            opts.format = screenshot_format_;
            opts.scale = screenshot_scale_;
            opts.crop_x = screenshot_x_;
            opts.crop_y = screenshot_y_;
            opts.crop_width = screenshot_width_;
            opts.crop_height = screenshot_height_;
            opts.show = screenshot_show_;

            return handler.handle_screenshot(capture_conn_id_, opts);
        }

        // Should not reach here as CLI11 ensures a subcommand is selected
        Result result;
        result.status = Result::Status::Error;
        result.error["code"] = "NO_SUBCOMMAND";
        result.error["message"] = "No screen subcommand specified (read, find, or capture)";
        return result;
    }

    bool was_invoked() const override {
        return screen_cmd_ && *screen_cmd_;
    }

    std::optional<std::string> get_preferred_output_format() const override {
        // Only override if screen read was invoked and an explicit output format was provided
        if (read_cmd_ && *read_cmd_ && !read_output_format_.empty()) {
            return read_output_format_;
        }
        if (find_cmd_ && *find_cmd_ && !find_output_format_.empty()) {
            return find_output_format_;
        }
        return std::nullopt;
    }

private:
    // Parent command
    CLI::App* screen_cmd_ = nullptr;

    // Read subcommand
    CLI::App* read_cmd_ = nullptr;
    bool read_children_ = true;
    bool no_tabs_ = false;
    std::string only_tab_;
    bool skip_trees_ = false;
    bool compact_ = false;
    bool probe_all_ = false;
    int max_rows_ = constants::MAX_TABLE_ROWS;
    int row_offset_ = 0;
    std::optional<int> read_conn_id_;
    std::string read_output_format_;

    // Targeted search subcommand
    CLI::App* find_cmd_ = nullptr;
    std::string find_id_contains_;
    std::string find_name_contains_;
    std::string find_type_;
    int find_limit_ = 1;
    bool find_probe_all_ = false;
    std::optional<int> find_conn_id_;
    std::string find_output_format_;

    // Filter options
    cli::ScreenFilterOptions filters_;
    std::string filter_text_contains_;
    std::string filter_id_contains_;
    std::string filter_type_;

    // Capture subcommand
    CLI::App* capture_cmd_ = nullptr;
    std::string screenshot_file_;
    std::string screenshot_format_ = "png";
    std::string screenshot_scale_;
    std::optional<int> screenshot_x_;
    std::optional<int> screenshot_y_;
    std::optional<int> screenshot_width_;
    std::optional<int> screenshot_height_;
    bool screenshot_show_ = false;
    std::optional<int> capture_conn_id_;
};

// Factory function
std::unique_ptr<CommandBase> create_screen_command() {
    return std::make_unique<ScreenCommand>();
}

} // namespace commands
} // namespace fairyfly
