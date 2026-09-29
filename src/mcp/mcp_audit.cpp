#include "include/mcp/mcp_audit.h"

namespace fairyfly::mcp {

// PHASE 3: real audit mapping (owner: phase 3 worker). Stub is a no-op.
AuditHook make_mcp_audit_hook(audit::AuditSink* sink,
                              const std::function<cli::CommandHandler*()>& peek_handler,
                              bool read_only) {
    (void)sink; (void)peek_handler; (void)read_only;
    return [](const McpCallRecord&) {};
}

} // namespace fairyfly::mcp
