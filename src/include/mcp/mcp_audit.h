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

/// True once any append made through a hook from make_mcp_audit_hook / append_serve_event failed
/// (thread-safe, sticky until reset). In audit Required mode the dispatcher / run_serve should
/// query this after a call and refuse further calls with AUDIT_UNAVAILABLE. In other modes a
/// failure only logs one spdlog warning (done by AuditSink) and never affects the call.
bool mcp_audit_failed() noexcept;
void mcp_audit_reset_failure() noexcept;

/// Appends one {cmd:"serve", source:"mcp", status} lifecycle record ("started" | "stopped").
/// No-op for a null/disabled sink. Returns false when the append failed.
bool append_serve_event(audit::AuditSink* sink, const std::string& status, bool read_only) noexcept;

} // namespace fairyfly::mcp
