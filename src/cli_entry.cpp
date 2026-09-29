#include <CLI/CLI.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>
#include <spdlog/sinks/stdout_color_sinks.h>
#include <iostream>
#include <fstream>
#include <functional>
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
#include "include/commands/batch_command.h"
#include "include/commands/mcp_command.h"
#include "include/commands/cli_app.h"
#include "include/mcp/run_mcp.h"
#include "include/mcp/tool_catalog.h"
#include "include/exceptions.h"
#include "include/version.h"
#include "include/audit_log.h"
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
        response["metadata"]["version"] = fairyfly::FAIRYFLY_VERSION;
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

namespace {
    /// Lazily yields the one CommandHandler shared by every command of this process.
    using HandlerProvider = std::function<CommandHandler&()>;

    int run_batch(std::string file, bool stop_on_error, const HandlerProvider& get_handler,
                  const GlobalOptions& batch_opts);

    /// Per-process audit context (set by run_cli). peek_handler never creates the handler,
    /// so --help and parse errors do not initialise COM just to be audited.
    struct AuditContext {
        audit::AuditSink* sink = nullptr;
        std::function<const CommandHandler*()> peek_handler;
    };
    AuditContext g_audit;
    /// Set by the `mcp` server path: the per-invocation audit record of run_one is skipped for it.
    bool g_skip_invocation_audit = false;

    void note_result(audit::AuditRecord& out, const Result& result) {
        if (result.status != Result::Status::Success) {
            if (result.error.is_object() && result.error.contains("code") && result.error["code"].is_string())
                out.error_code = result.error["code"].get<std::string>();
            else if (result.status == Result::Status::NotImplemented)
                out.error_code = "NOT_IMPLEMENTED";
        }
        if (result.data.is_object() && result.data.contains("connection_id")) {
            const auto& id = result.data["connection_id"];
            try {
                if (id.is_number_integer()) out.connection = id.get<int>();
                else if (id.is_string() && !id.get<std::string>().empty()) out.connection = std::stoi(id.get<std::string>());
            } catch (const std::exception&) {}
        }
    }

    /// The audit command is the parsed CLI path ("element click"); it comes from the command table paths.
    void note_command(audit::AuditRecord& out, const CLI::App& app) {
        out.command = invoked_command_path(app);
    }

    /// Fallback for `--connection N` when the result carried no connection id.
    void note_connection_from_argv(audit::AuditRecord& out, const std::vector<std::string>& argv) {
        if (out.connection) return;
        for (size_t i = 1; i + 1 < argv.size(); ++i) {
            if (argv[i] == "--connection") {
                try { out.connection = std::stoi(argv[i + 1]); } catch (const std::exception&) {}
                return;
            }
        }
    }

    /// True when FAIRYFLY_READ_ONLY is set to 1/true/yes/on.
    bool env_read_only() {
        std::string value;
#ifdef _WIN32
        char* env_buffer = nullptr;
        size_t env_size = 0;
        if (_dupenv_s(&env_buffer, &env_size, "FAIRYFLY_READ_ONLY") == 0 && env_buffer != nullptr) {
            value = env_buffer;
            free(env_buffer);
        }
#else
        if (const char* env_value = std::getenv("FAIRYFLY_READ_ONLY")) value = env_value;
#endif
        std::transform(value.begin(), value.end(), value.begin(), ::tolower);
        return value == "1" || value == "true" || value == "yes" || value == "on";
    }

    /// Print one JSON result on a single line (batch protocol).
    void print_compact_json(const json& j) {
        std::cout << j.dump() << std::endl;
    }

    /// Parse and execute one command line (argv[0] is the program name).
    /// The handler is injected lazily so parse-only invocations (--help, usage errors)
    /// never initialise COM. All CLI11/registry state is rebuilt on every call.
    /// batch_mode: one compact JSON object per call, never prints help, rejects nested batch.
    int run_one_impl(const std::vector<std::string>& argv, const HandlerProvider& get_handler,
                     GlobalOptions& global_opts, bool batch_mode, audit::AuditRecord& audit_out) {
    CLI::App app{"fairyfly - LLM-powered SAP GUI automation CLI"};
    app.set_version_flag("--version", fairyfly::FAIRYFLY_VERSION);

    add_global_options(app, global_opts);
    build_command_tree(app);

    // Parse arguments
    std::vector<const char*> arg_ptrs;
    arg_ptrs.reserve(argv.size());
    for (const auto& arg : argv) arg_ptrs.push_back(arg.c_str());
    try {
        app.parse(static_cast<int>(arg_ptrs.size()), arg_ptrs.data());
    } catch (const CLI::ParseError& e) {
        note_command(audit_out, app);
        if (!batch_mode) {
            const int exit_code = app.exit(e);
            if (exit_code != 0) audit_out.error_code = "PARSE_ERROR";
            return exit_code;
        }
        std::ostringstream out, err;
        const int code = app.exit(e, out, err);
        Result parse_result;
        if (code == 0) {
            parse_result.status = Result::Status::Success;
            parse_result.data["output"] = out.str();
        } else {
            std::string message = err.str();
            while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) message.pop_back();
            parse_result.status = Result::Status::Error;
            parse_result.error["code"] = "PARSE_ERROR";
            parse_result.error["message"] = message;
            audit_out.error_code = "PARSE_ERROR";
        }
        print_compact_json(parse_result.to_json());
        return code == 0 ? 0 : 1;
    }

    if (env_read_only()) global_opts.read_only = true;
    audit_out.read_only = global_opts.read_only;
    note_command(audit_out, app);
    note_connection_from_argv(audit_out, argv);

    // Setup logging
    setup_logging(global_opts.log_level);

    // Initialize element renderers (once per process)
    static bool renderers_registered = false;
    if (!renderers_registered) {
        sap::renderers::register_all_renderers();
        renderers_registered = true;
        spdlog::debug("Element renderers initialized");
    }

    // Execute command
    Result command_result;
    auto requested_output_format = [&]() {
        if (batch_mode) return OutputFormat::Json;
        std::string effective_format = global_opts.output_format;
        if (auto command_format = CommandRegistry::instance().get_active_command_output_format())
            effective_format = *command_format;
        if (effective_format == "markdown") return OutputFormat::Markdown;
        if (effective_format == "text" || effective_format == "plain") return OutputFormat::PlainText;
        if (effective_format == "toon") return OutputFormat::Toon;
        return OutputFormat::Json;
    };
    auto print_result = [&]() {
        std::string text = format_output(command_result, requested_output_format(), global_opts.verbose_errors);
        if (batch_mode) {
            // Keep the batch protocol strictly one JSON object per line.
            try { text = json::parse(text).dump(); } catch (const std::exception&) {}
        }
        std::cout << text << std::endl;
    };

    try {
        // `batch` runs the shared handler over many lines (see run_batch).
        for (const auto& command : CommandRegistry::instance().all_commands()) {
            auto* batch_command = dynamic_cast<BatchCommand*>(command.get());
            if (!batch_command || !batch_command->was_invoked()) continue;
            if (batch_mode) {
                command_result.status = Result::Status::Error;
                command_result.error["code"] = "BATCH_NESTED";
                command_result.error["message"] = "batch cannot be nested inside a batch";
                note_result(audit_out, command_result);
                print_result();
                return 1;
            }
            // Copy the options first: run_batch rebuilds the registry, destroying this command.
            return run_batch(batch_command->file(), batch_command->stop_on_error(), get_handler, global_opts);
        }

        // `mcp` is special-cased like `batch`: the server owns stdout for the protocol.
        for (const auto& command : CommandRegistry::instance().all_commands()) {
            auto* mcp_command = dynamic_cast<McpCommand*>(command.get());
            if (!mcp_command || !mcp_command->was_invoked()) continue;
            if (batch_mode) {
                command_result.status = Result::Status::Error;
                command_result.error["code"] = "MCP_UNAVAILABLE";
                command_result.error["message"] = "mcp cannot run inside a batch";
                note_result(audit_out, command_result);
                print_result();
                return 1;
            }
            if (mcp_command->tools_invoked()) {
                // Pure table output (no SAP access): plain text or Markdown, never JSON-wrapped.
                std::cout << mcp::tool_table_text(mcp_command->tools_markdown()) << std::flush;
                return 0;
            }
            // Copy the options first: nothing may rebuild the registry (and destroy this command) later.
            mcp::ServeOptions serve_options = mcp_command->options();
            if (const auto handled = run_mcp_extras(mcp_command->extras(), serve_options, get_handler, global_opts))
                return *handled;
            g_skip_invocation_audit = true;
            std::function<CommandHandler*()> peek = [] {
                return g_audit.peek_handler ? const_cast<CommandHandler*>(g_audit.peek_handler()) : nullptr;
            };
            return mcp::run_mcp(serve_options, get_handler, global_opts, g_audit.sink, peek);
        }

        CommandHandler& handler = get_handler();
        handler.set_read_only(global_opts.read_only);
        handler.set_batch_mode(batch_mode);  // credentials set/import-env cannot prompt inside `batch`
        command_result = CommandRegistry::instance().execute_active_command(handler);
        note_result(audit_out, command_result);

        // Check if no command was invoked
        if (command_result.status == Result::Status::Error &&
            command_result.error.contains("code") &&
            command_result.error["code"] == "NO_COMMAND") {
            if (batch_mode) {
                print_result();
                return 1;
            }
            audit_out.error_code.clear();
            std::cout << app.help() << std::endl;
            return 0;
        }

        // Add metadata
        json response = command_result.to_json();
        add_metadata(response);

        print_result();

    } catch (const UserError& e) {
        spdlog::error("User error: {}", e.what());
        command_result.status = Result::Status::Error;
        command_result.error["code"] = "USER_ERROR";
        command_result.error["message"] = e.what();
        note_result(audit_out, command_result);
        print_result();
        return 1;
    } catch (const SystemError& e) {
        spdlog::error("System error: {}", e.what());
        command_result.status = Result::Status::Error;
        command_result.error["code"] = "SYSTEM_ERROR";
        command_result.error["message"] = e.what();
        note_result(audit_out, command_result);
        print_result();
        return 1;
#ifdef _WIN32
    } catch (const _com_error& e) {
        command_result.status = Result::Status::Error;
        command_result.error["code"] = "COM_ERROR";
        std::ostringstream message;
        message << "COM operation failed (HRESULT 0x" << std::hex << std::uppercase
                << static_cast<unsigned long>(e.Error()) << ')';
        command_result.error["message"] = message.str();
        note_result(audit_out, command_result);
        print_result();
        return 1;
#endif
    } catch (const std::exception& e) {
        spdlog::error("Exception: {}", e.what());
        command_result.status = Result::Status::Error;
        command_result.error["code"] = "INTERNAL_ERROR";
        command_result.error["message"] = e.what();
        note_result(audit_out, command_result);
        print_result();
        return 1;
    } catch (...) {
        command_result.status = Result::Status::Error;
        command_result.error["code"] = "INTERNAL_ERROR";
        command_result.error["message"] = "Unknown failure while executing command";
        note_result(audit_out, command_result);
        print_result();
        return 1;
    }

    return command_result.status == Result::Status::Success ? 0 : 1;
    }

    /// Audited entry point: runs the command, then appends one audit record (never throws,
    /// never changes the exit code unless the opt-in --audit-required mode cannot write).
    int run_one(const std::vector<std::string>& argv, const HandlerProvider& get_handler,
                GlobalOptions& global_opts, bool batch_mode) {
        const auto started = std::chrono::steady_clock::now();
        audit::AuditRecord record;
        record.ts = std::chrono::system_clock::now();
        if (argv.size() > 1) record.argv.assign(argv.begin() + 1, argv.end());
        record.batch_line = global_opts.batch_line;

        int exit_code = run_one_impl(argv, get_handler, global_opts, batch_mode, record);

        if (g_audit.sink && g_audit.sink->enabled() && !g_skip_invocation_audit) {
            record.status = exit_code == 0 ? "success" : "error";
            record.exit_code = exit_code;
            record.duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
            if (g_audit.peek_handler) {
                if (const CommandHandler* handler = g_audit.peek_handler()) {
                    auto facts = handler->audit_facts();
                    if (facts.any()) record.sap = std::move(facts);
                }
            }
            if (!g_audit.sink->append(record) && g_audit.sink->mode() == audit::Mode::Required && exit_code == 0) {
                spdlog::error("AUDIT_UNAVAILABLE: command ran but its audit record could not be written");
                exit_code = 1;
            }
        }
        return exit_code;
    }

    /// Batch loop: one line -> one command -> one compact JSON line, all on the shared handler.
    int run_batch(std::string file, bool stop_on_error, const HandlerProvider& get_handler,
                  const GlobalOptions& batch_opts) {
        std::ifstream file_stream;
        std::istream* in = &std::cin;
        if (!file.empty()) {
            file_stream.open(file);
            if (!file_stream) {
                Result failure;
                failure.status = Result::Status::Error;
                failure.error["code"] = "BATCH_FILE_ERROR";
                failure.error["message"] = "Cannot open batch file: " + file;
                print_compact_json(failure.to_json());
                return 1;
            }
            in = &file_stream;
        }

        int failures = 0;
        size_t line_number = 0;
        std::string line;
        while (std::getline(*in, line)) {
            ++line_number;
            const BatchLine parsed = parse_batch_line(line);
            if (parsed.skip) continue;

            int exit_code = 0;
            if (!parsed.ok) {
                Result failure;
                failure.status = Result::Status::Error;
                failure.error["code"] = "BATCH_PARSE_ERROR";
                failure.error["message"] = parsed.error;
                failure.error["line"] = line_number;
                print_compact_json(failure.to_json());
                exit_code = 1;
            } else {
                // Fresh options per line; only the batch-level guard/verbosity carry over.
                GlobalOptions line_opts;
                line_opts.read_only = batch_opts.read_only;
                line_opts.verbose_errors = batch_opts.verbose_errors;
                line_opts.log_level = batch_opts.log_level;
                line_opts.batch_line = line_number;
                std::vector<std::string> argv{"fairyfly"};
                argv.insert(argv.end(), parsed.argv.begin(), parsed.argv.end());
                exit_code = run_one(argv, get_handler, line_opts, true);
            }
            if (exit_code != 0) {
                ++failures;
                if (stop_on_error) break;
            }
        }
        return failures == 0 ? 0 : 1;
    }
}

int fairyfly::cli::run_cli(int argc, char** argv) {
    std::unique_ptr<CommandHandler> handler;
    HandlerProvider get_handler = [&handler]() -> CommandHandler& {
        if (!handler) handler = std::make_unique<CommandHandler>();
        return *handler;
    };

    // Audit trail: flags are pre-scanned so the sink exists before parsing (parse errors are audited too).
    bool cli_no_audit = false, cli_required = false;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i] ? argv[i] : "";
        if (arg == "--no-audit") cli_no_audit = true;
        else if (arg == "--audit-required") cli_required = true;
    }
    auto env_lookup = [](const char* name) -> std::string {
        char* buffer = nullptr;
        size_t size = 0;
        std::string value;
        if (_dupenv_s(&buffer, &size, name) == 0 && buffer != nullptr) { value = buffer; free(buffer); }
        return value;
    };
    audit::AuditSink sink(audit::resolve_config(env_lookup, cli_no_audit, cli_required,
                                                std::chrono::system_clock::now()));
    struct ContextGuard {
        ~ContextGuard() { g_audit = AuditContext{}; }
    } context_guard;
    g_audit.sink = &sink;
    g_audit.peek_handler = [&handler]() -> const CommandHandler* { return handler.get(); };

    if (const auto refusal = audit::preflight_error(sink.mode(), sink.mode() != audit::Mode::Required || sink.probe())) {
        Result failure;
        failure.status = Result::Status::Error;
        failure.error["code"] = *refusal;
        failure.error["message"] = "Audit trail is required but cannot be written: " + sink.file().string();
        print_compact_json(failure.to_json());
        return 1;
    }

    GlobalOptions global_opts;
    return run_one(std::vector<std::string>(argv, argv + argc), get_handler, global_opts, false);
}
