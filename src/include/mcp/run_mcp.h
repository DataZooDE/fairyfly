#pragma once
#include <functional>
#include "include/audit_log.h"
#include "include/cli_handler.h"
#include "include/commands/global_options.h"
#include "include/mcp/types.h"

namespace fairyfly::mcp {

/// Optional hooks for embedding the HTTP server (tray runner): receive its IServerControl, learn whether the run
/// ended because of request_restart(), and keep the caller's logging setup.
struct HttpRunHooks {
    std::function<void(IServerControl&)> on_control;
    bool* restart_requested = nullptr;
    bool keep_logging = false;
};

/// Validates options, wires policy/audit/dispatcher/transport/server and runs the MCP server.
/// Returns the process exit code (2 = refused to start / invalid options).
/// `sink` may be null (no audit); `peek_handler` may be empty (never creates the handler).
int run_mcp(const ServeOptions& options, const std::function<cli::CommandHandler&()>& get_handler,
              const commands::GlobalOptions& global, audit::AuditSink* sink = nullptr,
              const std::function<cli::CommandHandler*()>& peek_handler = {}, const HttpRunHooks* hooks = nullptr);

} // namespace fairyfly::mcp
