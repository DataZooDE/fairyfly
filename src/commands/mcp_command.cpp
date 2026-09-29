#include "include/commands/mcp_command.h"

#include <memory>

#include "include/command_table.h"

namespace fairyfly {
namespace commands {

CLI::App* McpCommand::setup_cli(CLI::App& app) {
    cmd_ = noun_app(app, "mcp");
    cmd_->add_flag("--read-only", options_.read_only,
                   "Read-only guard mode (default): state-changing actions are refused, write tools are hidden; FAIRYFLY_READ_ONLY=1 forces it");
    cmd_->add_flag("--allow-write", options_.allow_write,
                   "Enable write mode: expose gui_element_fill, allow multiple_logon=end and close_session, turn the read-only guard off");
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
    cmd_->add_option("--tools", tools_filter_,
                     "Only expose the tools of these families (comma or space separated), e.g. "
                     "\"session,screen,element\". Families: " + [] {
                         std::string out;
                         for (const auto& family : command_table::families()) out += (out.empty() ? "" : ", ") + family;
                         return out;
                     }() + ". Default: all families");

    tools_cmd_ = add_leaf(app, {"mcp", "tools"});
    tools_cmd_->add_flag("--markdown", tools_markdown_, "Print the tool table as Markdown");
    setup_mcp_extras(*cmd_, extras_);
    return cmd_;
}

Result McpCommand::execute(cli::CommandHandler& handler) {
    (void)handler;
    // cli_entry.cpp calls mcp::run_mcp() for this command; reaching here means the registry
    // was executed directly, which is not a supported entry point.
    Result result;
    result.status = Result::Status::Error;
    result.error["code"] = "MCP_UNAVAILABLE";
    result.error["message"] = "MCP server is not available through this entry point";
    return result;
}

mcp::ServeOptions McpCommand::options() const {
    mcp::ServeOptions out = options_;
    if (default_connection_ >= 0) out.default_connection = default_connection_;
    out.families = command_table::parse_family_list(tools_filter_);
    return out;
}

// Factory function
std::unique_ptr<CommandBase> create_mcp_command() {
    return std::make_unique<McpCommand>();
}

} // namespace commands
} // namespace fairyfly
