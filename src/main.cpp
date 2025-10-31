#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <iostream>
#include <string>
#include <memory>
#include <chrono>
#include <iomanip>
#include "include/core.h"
#include "include/cli_handler.h"
#include "include/element_renderers.h"

using json = nlohmann::json;
using namespace fairyfly;
using namespace fairyfly::cli;

int main(int argc, char** argv) {
    CLI::App app{"fairyfly - LLM-powered SAP GUI automation CLI"};
    app.set_version_flag("--version", "0.1.0");

    // Global options
    bool verbose = false;
    app.add_flag("-v,--verbose", verbose, "Enable verbose logging");

    std::string output_format = "json";
    app.add_option("--output", output_format, "Output format: json, markdown, yaml")
        ->check(CLI::IsMember({"json", "markdown", "yaml"}));

    // Subcommands

    // Attach command - click on running SAP window
    auto* attach_cmd = app.add_subcommand("attach", "Attach to running SAP GUI window");
    int attach_timeout = 20;
    attach_cmd->add_option("--timeout", attach_timeout, "Timeout in seconds for window selection")
        ->check(CLI::PositiveNumber);

    // Launch command - open SAP Logon connection
    auto* launch_cmd = app.add_subcommand("launch", "Launch SAP Logon connection");
    std::string connection_name;
    launch_cmd->add_option("connection", connection_name, "Connection name (e.g., PRD, DEV)")
        ->required();

    // Disconnect command
    auto* disconnect_cmd = app.add_subcommand("disconnect", "Disconnect from SAP system");
    std::optional<int> disconnect_conn_id;
    disconnect_cmd->add_option("--connection", disconnect_conn_id, "Connection ID to disconnect");

    // Connections command - list and manage connection files
    auto* connections_cmd = app.add_subcommand("connections", "List and manage SAP connections");
    bool cleanup = false;
    connections_cmd->add_flag("--cleanup", cleanup, "Remove invalid connection files");

    // Transaction command
    auto* tcode_cmd = app.add_subcommand("tcode", "Execute SAP transaction");
    std::string tcode;
    std::optional<int> tcode_conn_id;
    tcode_cmd->add_option("code", tcode, "Transaction code (e.g., SE38, VA01)")
        ->required();
    tcode_cmd->add_option("--connection", tcode_conn_id, "Connection ID to use");

    // Click command
    auto* click_cmd = app.add_subcommand("click", "Click UI element");
    std::string click_element;
    std::optional<int> click_conn_id;
    click_cmd->add_option("element", click_element, "Element ID (e.g., wnd[0]/usr/btn[3])")
        ->required();
    click_cmd->add_option("--connection", click_conn_id, "Connection ID to use");

    // Fill command
    auto* fill_cmd = app.add_subcommand("fill", "Fill text field");
    std::string fill_element, fill_value;
    std::optional<int> fill_conn_id;
    fill_cmd->add_option("element", fill_element, "Element ID")
        ->required();
    fill_cmd->add_option("value", fill_value, "Value to enter")
        ->required();
    fill_cmd->add_option("--connection", fill_conn_id, "Connection ID to use");

    // Get command
    auto* get_cmd = app.add_subcommand("get", "Read field value");
    std::string get_element;
    std::optional<int> get_conn_id;
    get_cmd->add_option("element", get_element, "Element ID")
        ->required();
    get_cmd->add_option("--connection", get_conn_id, "Connection ID to use");

    // Screen command group
    auto* screen_cmd = app.add_subcommand("screen", "Screen operations");
    std::optional<int> screen_conn_id;
    auto* screen_read = screen_cmd->add_subcommand("read", "Read screen structure");
    bool screen_read_children = true;
    bool no_tabs = false;     // Flag to disable tab expansion (default: tabs are expanded)
    std::string screen_output_format = "json";
    screen_read->add_flag("--no-children", screen_read_children, "Don't include child elements");
    screen_read->add_flag("--no-tabs", no_tabs, "Skip tab expansion (faster, less complete)");
    screen_read->add_option("--connection", screen_conn_id, "Connection ID to use");
    screen_read->add_option("--output", screen_output_format, "Output format: json, markdown")
        ->check(CLI::IsMember({"json", "markdown"}));
    auto* screen_capture = screen_cmd->add_subcommand("capture", "Capture screenshot");
    std::string screenshot_file;
    std::string screenshot_format = "png";
    std::string screenshot_scale;
    std::optional<int> screenshot_x;
    std::optional<int> screenshot_y;
    std::optional<int> screenshot_width;
    std::optional<int> screenshot_height;
    bool screenshot_show = false;

    screen_capture->add_option("--file,-f", screenshot_file, "Output file path or '-' for stdout");
    screen_capture->add_option("--format", screenshot_format, "Output format: png, base64")
        ->check(CLI::IsMember({"png", "base64"}));
    screen_capture->add_option("--scale", screenshot_scale, "Scale factor (0.0-1.0) or width in pixels");
    screen_capture->add_option("--x", screenshot_x, "X position for subsection capture (pixels)");
    screen_capture->add_option("--y", screenshot_y, "Y position for subsection capture (pixels)");
    screen_capture->add_option("--width", screenshot_width, "Width for subsection capture (pixels)");
    screen_capture->add_option("--height", screenshot_height, "Height for subsection capture (pixels)");
    screen_capture->add_flag("--show", screenshot_show, "Display screenshot in window after capture");
    screen_capture->add_option("--connection", screen_conn_id, "Connection ID to use");

    // List command - enumerate all SAP connections, sessions, windows
    auto* list_cmd = app.add_subcommand("list", "List all SAP GUI connections, sessions, and windows");

    // MCP server mode (for Phase 3)
    auto* serve_cmd = app.add_subcommand("serve", "Start MCP server");
    std::string transport = "stdio";
    int port = 8080;
    serve_cmd->add_option("--transport", transport, "Transport: stdio, http")
        ->check(CLI::IsMember({"stdio", "http"}));
    serve_cmd->add_option("--port", port, "HTTP port (when transport=http)");

    // Parse arguments
    CLI11_PARSE(app, argc, argv);

    // Configure logging - use stderr to avoid mixing with JSON/markdown output
    auto console = spdlog::stderr_color_mt("console");
    spdlog::set_default_logger(console);

    if (verbose) {
        spdlog::set_level(spdlog::level::debug);
        spdlog::debug("Verbose logging enabled");
    } else {
        spdlog::set_level(spdlog::level::info);
    }

    // Initialize element renderers (registry pattern for type-based dispatch)
    sap::renderers::register_all_renderers();
    spdlog::debug("Element renderers initialized");

    // Create command handler
    auto handler = std::make_unique<CommandHandler>();

    // Command handlers
    Result command_result;

    try {
        if (*attach_cmd) {
            command_result = handler->handle_attach(attach_timeout);
        }
        else if (*launch_cmd) {
            command_result = handler->handle_launch(connection_name);
        }
        else if (*disconnect_cmd) {
            command_result = handler->handle_disconnect(disconnect_conn_id);
        }
        else if (*connections_cmd) {
            command_result = handler->handle_connections_list(cleanup);
        }
        else if (*tcode_cmd) {
            command_result = handler->handle_transaction(tcode, tcode_conn_id);
        }
        else if (*click_cmd) {
            command_result = handler->handle_click(click_element, click_conn_id);
        }
        else if (*fill_cmd) {
            command_result = handler->handle_fill(fill_element, fill_value, fill_conn_id);
        }
        else if (*get_cmd) {
            command_result = handler->handle_read_field(get_element, get_conn_id);
        }
        else if (*screen_read) {
            // Apply --no-tabs flag: if set, disable tab expansion
            // Clearer logic: expand_tabs defaults to true, --no-tabs flag disables it
            bool should_expand_tabs = !no_tabs;
            command_result = handler->handle_screen_read(screen_read_children, screen_conn_id, should_expand_tabs);
        }
        else if (*screen_capture) {
            ScreenshotOptions screenshot_opts;
            screenshot_opts.output_file = screenshot_file;
            screenshot_opts.format = screenshot_format;
            screenshot_opts.scale = screenshot_scale;
            screenshot_opts.crop_x = screenshot_x;
            screenshot_opts.crop_y = screenshot_y;
            screenshot_opts.crop_width = screenshot_width;
            screenshot_opts.crop_height = screenshot_height;
            screenshot_opts.show = screenshot_show;
            command_result = handler->handle_screenshot(screen_conn_id, screenshot_opts);
        }
        else if (*list_cmd) {
            command_result = handler->handle_list_all();
        }
        else if (*serve_cmd) {
            spdlog::info("Starting MCP server with transport: {}", transport);
            if (transport == "http") {
                spdlog::info("HTTP server on port: {}", port);
            }
            command_result.status = Result::Status::Error;
            command_result.error["code"] = "NOT_IMPLEMENTED";
            command_result.error["message"] = "MCP server implementation coming in Phase 3";
        }
        else {
            // No subcommand specified
            std::cout << app.help() << std::endl;
            return 0;
        }

        // Get current timestamp
        auto now = std::chrono::system_clock::now();
        auto time_t = std::chrono::system_clock::to_time_t(now);
        std::tm tm_buf;
#ifdef _WIN32
        gmtime_s(&tm_buf, &time_t);
#else
        gmtime_r(&time_t, &tm_buf);
#endif
        std::stringstream ss;
        ss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%SZ");

        // Convert result to JSON and add metadata
        json response = command_result.to_json();
        if (!response.contains("metadata")) {
            response["metadata"] = json::object();
        }
        response["metadata"]["timestamp"] = ss.str();
        response["metadata"]["version"] = "0.1.0";

        // Output response in requested format
        OutputFormat fmt = OutputFormat::Json;

        // Use screen-specific output format if screen read was executed
        std::string effective_output_format = output_format;
        if (*screen_read) {
            effective_output_format = screen_output_format;
        }

        if (effective_output_format == "markdown") {
            fmt = OutputFormat::Markdown;
        } else if (effective_output_format == "text" || effective_output_format == "plain") {
            fmt = OutputFormat::PlainText;
        }

        std::cout << format_output(command_result, fmt) << std::endl;

    } catch (const std::exception& e) {
        spdlog::error("Exception: {}", e.what());
        command_result.status = Result::Status::Error;
        command_result.error["code"] = "INTERNAL_ERROR";
        command_result.error["message"] = e.what();
        std::cout << format_output(command_result, OutputFormat::Json) << std::endl;
        return 1;
    }

    return command_result.status == Result::Status::Success ? 0 : 1;
}
