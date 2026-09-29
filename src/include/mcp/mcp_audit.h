#pragma once
#include <functional>
#include "include/audit_log.h"
#include "include/cli_handler.h"
#include "include/mcp/types.h"

namespace fairyfly::mcp {

/// Builds the AuditHook that appends one audit::AuditRecord per MCP call to `sink`
/// (nullptr or disabled sink => no-op hook). `peek_handler` returns the handler if it already
/// exists (never creates it) so SAP facts can be attached.
AuditHook make_mcp_audit_hook(audit::AuditSink* sink,
                              const std::function<cli::CommandHandler*()>& peek_handler,
                              bool read_only);

} // namespace fairyfly::mcp
