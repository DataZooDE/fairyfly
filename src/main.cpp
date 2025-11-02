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
#include "include/commands/command_registry.h"
#include "include/commands/global_options.h"
#include "include/exceptions.h"

using json = nlohmann::json;
using namespace fairyfly;
using namespace fairyfly::cli;
using namespace fairyfly::commands;

namespace {
    /// Add timestamp and version metadata to result
    void add_metadata(json& response) {
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

        if (!response.contains("metadata")) {
            response["metadata"] = json::object();
        }
        response["metadata"]["timestamp"] = ss.str();
        response["metadata"]["version"] = "0.1.0";
    }

    /// Setup logging based on verbose flag
    void setup_logging(bool verbose) {
        auto console = spdlog::stderr_color_mt("console");
        spdlog::set_default_logger(console);

        if (verbose) {
            spdlog::set_level(spdlog::level::debug);
            spdlog::debug("Verbose logging enabled");
        } else {
            spdlog::set_level(spdlog::level::info);
        }
    }
}

int main(int argc, char** argv) {
    CLI::App app{"fairyfly - LLM-powered SAP GUI automation CLI"};
    app.set_version_flag("--version", "0.1.0");

    // Global options
    GlobalOptions global_opts;
    app.add_flag("-v,--verbose", global_opts.verbose, "Enable verbose logging");
    app.add_option("--output", global_opts.output_format, "Output format: json, markdown, toon")
        ->check(CLI::IsMember({"json", "markdown", "toon"}));

    // Register all commands explicitly
    register_all_commands();
    CommandRegistry::instance().setup_all_commands(app);

    // Parse arguments
    CLI11_PARSE(app, argc, argv);

    // Setup logging
    setup_logging(global_opts.verbose);

    // Initialize element renderers
    sap::renderers::register_all_renderers();
    spdlog::debug("Element renderers initialized");

    // Create command handler
    auto handler = std::make_unique<CommandHandler>();

    // Execute command
    Result command_result;

    try {
        command_result = CommandRegistry::instance().execute_active_command(*handler);

        // Check if no command was invoked
        if (command_result.status == Result::Status::Error &&
            command_result.error.contains("code") &&
            command_result.error["code"] == "NO_COMMAND") {
            std::cout << app.help() << std::endl;
            return 0;
        }

        // Add metadata
        json response = command_result.to_json();
        add_metadata(response);

        // Output in requested format
        // Check if active command has output format preference (e.g., screen read --output)
        std::string effective_format = global_opts.output_format;
        auto cmd_format = CommandRegistry::instance().get_active_command_output_format();
        if (cmd_format.has_value()) {
            effective_format = cmd_format.value();
        }

        // Convert to OutputFormat enum
        OutputFormat fmt = OutputFormat::Json;
        if (effective_format == "markdown") {
            fmt = OutputFormat::Markdown;
        } else if (effective_format == "text" || effective_format == "plain") {
            fmt = OutputFormat::PlainText;
        } else if (effective_format == "toon") {
            fmt = OutputFormat::Toon;
        }

        std::cout << format_output(command_result, fmt) << std::endl;

    } catch (const UserError& e) {
        spdlog::error("User error: {}", e.what());
        command_result.status = Result::Status::Error;
        command_result.error["code"] = "USER_ERROR";
        command_result.error["message"] = e.what();
        std::cout << format_output(command_result, OutputFormat::Json) << std::endl;
        return 1;
    } catch (const SystemError& e) {
        spdlog::error("System error: {}", e.what());
        command_result.status = Result::Status::Error;
        command_result.error["code"] = "SYSTEM_ERROR";
        command_result.error["message"] = e.what();
        std::cout << format_output(command_result, OutputFormat::Json) << std::endl;
        return 1;
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
