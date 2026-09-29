#pragma once
// Adapter between the HTTP server (run_mcp) and the tray (tray::IServerRunner).

#include <functional>
#include <memory>

#include "include/audit_log.h"
#include "include/cli_handler.h"
#include "include/commands/global_options.h"
#include "include/mcp/types.h"
#include "include/tray/tray.h"

namespace fairyfly::mcp {

std::unique_ptr<tray::IServerRunner> make_http_tray_runner(const ServeOptions& options,
                                                           std::function<cli::CommandHandler&()> get_handler,
                                                           const commands::GlobalOptions& global, audit::AuditSink* sink,
                                                           std::function<cli::CommandHandler*()> peek);

} // namespace fairyfly::mcp
