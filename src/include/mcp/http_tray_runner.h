#pragma once
// Adapter between the HTTP server (run_mcp) and the tray (tray::IServerRunner).

#include <functional>
#include <memory>

#include "include/audit_log.h"
#include "include/cli_handler.h"
#include "include/commands/global_options.h"
#include "include/mcp/run_mcp.h"
#include "include/mcp/types.h"
#include "include/tray/tray.h"

namespace fairyfly::mcp {

std::unique_ptr<tray::IServerRunner> make_http_tray_runner(const ServeOptions& options,
                                                           std::function<cli::CommandHandler&()> get_handler,
                                                           const commands::GlobalOptions& global, audit::AuditSink* sink,
                                                           std::function<cli::CommandHandler*()> peek);

/// Test seam: the function that runs one server instance (default: run_mcp).
using RunMcpFunction = std::function<int(const ServeOptions&, const std::function<cli::CommandHandler&()>&,
                                         const commands::GlobalOptions&, audit::AuditSink*,
                                         const std::function<cli::CommandHandler*()>&, const HttpRunHooks*)>;

std::unique_ptr<tray::IServerRunner> make_http_tray_runner(const ServeOptions& options,
                                                           std::function<cli::CommandHandler&()> get_handler,
                                                           const commands::GlobalOptions& global, audit::AuditSink* sink,
                                                           std::function<cli::CommandHandler*()> peek,
                                                           RunMcpFunction run_fn);

} // namespace fairyfly::mcp
