#pragma once
#include <functional>
#include <string>
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

/// True when the most recent append made through a hook or append_serve_event failed.
/// Thread-safe; a successful denial append clears the failure and permits a later
/// request in Required mode. An action that already ran still reports
/// AUDIT_UNAVAILABLE when its own append fails. In other modes failures only log.
bool mcp_audit_failed() noexcept;
void mcp_audit_reset_failure() noexcept;

/// Appends one {cmd:"mcp", source:"mcp", status} lifecycle record ("started" | "stopped").
/// No-op for a null/disabled sink. Returns false when the append failed.
bool append_serve_event(audit::AuditSink* sink, const std::string& status, bool read_only) noexcept;

} // namespace fairyfly::mcp
