#include "include/commands/command_base.h"
#include "include/cli_handler.h"
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
        screen_cmd_ = app.add_subcommand(name(), description());

        // Add "read" subcommand
        read_cmd_ = screen_cmd_->add_subcommand("read", "Read screen structure");
        read_cmd_->add_flag("--no-children", read_children_, "Don't include child elements");
        read_cmd_->add_flag("--no-tabs", no_tabs_, "Skip tab expansion (faster, less complete)");
        read_cmd_->add_option("--connection", read_conn_id_, "Connection ID to use");
        read_cmd_->add_option("--output", read_output_format_, "Output format: json, markdown, toon")
            ->check(CLI::IsMember({"json", "markdown", "toon"}));

        // Add "capture" subcommand
        capture_cmd_ = screen_cmd_->add_subcommand("capture", "Capture screenshot");
        capture_cmd_->add_option("--file,-f", screenshot_file_, "Output file path or '-' for stdout");
        capture_cmd_->add_option("--format", screenshot_format_, "Output format: png, base64")
            ->check(CLI::IsMember({"png", "base64"}));
        capture_cmd_->add_option("--scale", screenshot_scale_, "Scale factor (0.0-1.0) or width in pixels");
        capture_cmd_->add_option("--x", screenshot_x_, "X position for subsection capture (pixels)");
        capture_cmd_->add_option("--y", screenshot_y_, "Y position for subsection capture (pixels)");
        capture_cmd_->add_option("--width", screenshot_width_, "Width for subsection capture (pixels)");
        capture_cmd_->add_option("--height", screenshot_height_, "Height for subsection capture (pixels)");
        capture_cmd_->add_flag("--show", screenshot_show_, "Display screenshot in window after capture");
        capture_cmd_->add_option("--connection", capture_conn_id_, "Connection ID to use");

        return screen_cmd_;
    }

    Result execute(cli::CommandHandler& handler) override {
        if (*read_cmd_) {
            bool should_expand_tabs = !no_tabs_;
            return handler.handle_screen_read(read_children_, read_conn_id_, should_expand_tabs);
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
        result.error["message"] = "No screen subcommand specified (read or capture)";
        return result;
    }

    bool was_invoked() const override {
        return screen_cmd_ && *screen_cmd_;
    }

    std::optional<std::string> get_preferred_output_format() const override {
        // Only override if screen read was invoked (screen capture uses global format)
        if (read_cmd_ && *read_cmd_) {
            return read_output_format_;
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
    std::optional<int> read_conn_id_;
    std::string read_output_format_ = "markdown";

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
