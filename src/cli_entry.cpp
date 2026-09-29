#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <iostream>
#include <string>
#include <memory>
#include <chrono>
#include <iomanip>
#include <sstream>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>
#include "include/com/utf8.h"
#include "include/core.h"
#include "include/cli_handler.h"
#include "include/cli_entry.h"
#include "include/element_renderers.h"
#include "include/commands/command_registry.h"
#include "include/commands/global_options.h"
#include "include/exceptions.h"
#ifdef _WIN32
#include <windows.h>
#include <comdef.h>
#endif

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

    /// Convert log level string to spdlog::level::level_enum
    spdlog::level::level_enum parse_log_level(const std::string& level_str) {
        std::string lower = level_str;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);

        if (lower == "trace") return spdlog::level::trace;
        if (lower == "debug") return spdlog::level::debug;
        if (lower == "info") return spdlog::level::info;
        if (lower == "warn" || lower == "warning") return spdlog::level::warn;
        if (lower == "error" || lower == "err") return spdlog::level::err;
        if (lower == "critical" || lower == "crit") return spdlog::level::critical;
        if (lower == "off") return spdlog::level::off;

        // Keep invalid environment overrides as quiet as the CLI default.
        return spdlog::level::err;
    }

    /// Setup logging based on log level string
    /// Checks FFLYLOG_LEVEL environment variable first, then uses provided level
    void setup_logging(const std::string& log_level) {
        auto console = spdlog::get("console");
        if (!console) console = spdlog::stderr_color_mt("console");
        spdlog::set_default_logger(console);

        // Check environment variable first
        std::string effective_level = log_level;
#ifdef _WIN32
        char* env_buffer = nullptr;
        size_t env_size = 0;
        if (_dupenv_s(&env_buffer, &env_size, "FFLYLOG_LEVEL") == 0 && env_buffer != nullptr) {
            effective_level = env_buffer;
            free(env_buffer);
        }
#else
        const char* env_level = std::getenv("FFLYLOG_LEVEL");
        if (env_level != nullptr && std::strlen(env_level) > 0) {
            effective_level = env_level;
        }
#endif

        spdlog::level::level_enum level = parse_log_level(effective_level);
        spdlog::set_level(level);

        if (level == spdlog::level::debug || level == spdlog::level::trace) {
            spdlog::debug("Logging level set to: {}", effective_level);
        }
    }
}

int fairyfly::cli::run_cli(int argc, char** argv) {
    CLI::App app{"fairyfly - LLM-powered SAP GUI automation CLI"};
    app.set_version_flag("--version", "0.1.0");

    // Disable Windows-style options (/opt) to allow SAP element IDs starting with /
    // SAP element paths like /app/con[0]/ses[0]/wnd[0]/usr/txtField would otherwise
    // be interpreted as option flags on Windows, causing argument parsing failures
    app.allow_windows_style_options(false);

    // Global options
    GlobalOptions global_opts;
    app.add_option("--log-level", global_opts.log_level, "Set logging level: trace, debug, info, warn, error (default), critical, off")
        ->check(CLI::IsMember({"trace", "debug", "info", "warn", "warning", "error", "err", "critical", "crit", "off"}));
    app.add_flag("-v,--verbose", [&global_opts](std::int64_t) { global_opts.log_level = "debug"; }, "Shorthand for --log-level debug");
    app.add_option("--output", global_opts.output_format, "Output format: json (default), markdown, toon")
        ->check(CLI::IsMember({"json", "markdown", "toon"}));
    app.add_flag("--verbose-errors", global_opts.verbose_errors, "Include detailed error suggestions (default: compact errors)");

    // Register all commands explicitly
    register_all_commands();
    CommandRegistry::instance().setup_all_commands(app);

    // Parse arguments
    CLI11_PARSE(app, argc, argv);

    // Setup logging
    setup_logging(global_opts.log_level);

    // Initialize element renderers
    sap::renderers::register_all_renderers();
    spdlog::debug("Element renderers initialized");

    // Execute command
    Result command_result;
    auto requested_output_format = [&]() {
        std::string effective_format = global_opts.output_format;
        if (auto command_format = CommandRegistry::instance().get_active_command_output_format())
            effective_format = *command_format;
        if (effective_format == "markdown") return OutputFormat::Markdown;
        if (effective_format == "text" || effective_format == "plain") return OutputFormat::PlainText;
        if (effective_format == "toon") return OutputFormat::Toon;
        return OutputFormat::Json;
    };

    try {
        auto handler = std::make_unique<CommandHandler>();
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

        std::cout << format_output(command_result, requested_output_format(), global_opts.verbose_errors) << std::endl;

    } catch (const UserError& e) {
        spdlog::error("User error: {}", e.what());
        command_result.status = Result::Status::Error;
        command_result.error["code"] = "USER_ERROR";
        command_result.error["message"] = e.what();
        std::cout << format_output(command_result, requested_output_format(), global_opts.verbose_errors) << std::endl;
        return 1;
    } catch (const SystemError& e) {
        spdlog::error("System error: {}", e.what());
        command_result.status = Result::Status::Error;
        command_result.error["code"] = "SYSTEM_ERROR";
        command_result.error["message"] = e.what();
        std::cout << format_output(command_result, requested_output_format(), global_opts.verbose_errors) << std::endl;
        return 1;
#ifdef _WIN32
    } catch (const _com_error& e) {
        command_result.status = Result::Status::Error;
        command_result.error["code"] = "COM_ERROR";
        std::ostringstream message;
        message << "COM operation failed (HRESULT 0x" << std::hex << std::uppercase
                << static_cast<unsigned long>(e.Error()) << ')';
        command_result.error["message"] = message.str();
        std::cout << format_output(command_result, requested_output_format(), global_opts.verbose_errors) << std::endl;
        return 1;
#endif
    } catch (const std::exception& e) {
        spdlog::error("Exception: {}", e.what());
        command_result.status = Result::Status::Error;
        command_result.error["code"] = "INTERNAL_ERROR";
        command_result.error["message"] = e.what();
        std::cout << format_output(command_result, requested_output_format(), global_opts.verbose_errors) << std::endl;
        return 1;
    } catch (...) {
        command_result.status = Result::Status::Error;
        command_result.error["code"] = "INTERNAL_ERROR";
        command_result.error["message"] = "Unknown failure while executing command";
        std::cout << format_output(command_result, requested_output_format(), global_opts.verbose_errors) << std::endl;
        return 1;
    }

    return command_result.status == Result::Status::Success ? 0 : 1;
}
