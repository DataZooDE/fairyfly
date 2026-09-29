#include "include/commands/serve_command.h"

#include <memory>

namespace fairyfly {
namespace commands {

CLI::App* ServeCommand::setup_cli(CLI::App& app) {
    cmd_ = app.add_subcommand(name(), description());
    cmd_->add_flag("--read-only", options_.read_only,
                   "Read-only guard mode (default): state-changing actions are refused, write tools are hidden; FAIRYFLY_READ_ONLY=1 forces it");
    cmd_->add_flag("--allow-write", options_.allow_write,
                   "Enable write mode: expose sap_fill, allow multiple_logon=end and close_session, turn the read-only guard off");
    cmd_->add_option("--default-connection", default_connection_,
                     "Connection index used when a tool call omits 'connection'");
    cmd_->add_option("--format", options_.format, "Text format of tool results: markdown (default), json")
        ->check(CLI::IsMember({"markdown", "json"}));
    cmd_->add_option("--max-result-chars", options_.max_result_chars,
                     "Truncate text results beyond this many characters (default 60000)");
    cmd_->add_option("--max-image-bytes", options_.max_image_bytes,
                     "Refuse images larger than this many bytes (default 2097152)");
    cmd_->add_option("--max-calls-per-minute", options_.max_calls_per_minute,
                     "Rate limit for tool calls (default 120)");
    cmd_->add_option("--call-timeout-ms", options_.call_timeout_ms,
                     "Soft timeout per tool call in milliseconds (default 120000)");
    cmd_->add_option("--transport", options_.transport, "Transport: stdio (http is not implemented)")
        ->check(CLI::IsMember({"stdio", "http"}));
    cmd_->add_option("--port", options_.port, "HTTP port (reserved; transport=http is not implemented)");
    return cmd_;
}

Result ServeCommand::execute(cli::CommandHandler& handler) {
    (void)handler;
    // PHASE 1: cli_entry.cpp calls mcp::run_serve() for this command; reaching here means the
    // registry was executed directly (e.g. inside `batch`).
    Result result;
    result.status = Result::Status::Error;
    result.error["code"] = "NOT_IMPLEMENTED";
    result.error["message"] = "MCP server is not available through this entry point";
    return result;
}

mcp::ServeOptions ServeCommand::options() const {
    mcp::ServeOptions out = options_;
    if (default_connection_ >= 0) out.default_connection = default_connection_;
    return out;
}

// Factory function
std::unique_ptr<CommandBase> create_serve_command() {
    return std::make_unique<ServeCommand>();
}

} // namespace commands
} // namespace fairyfly
