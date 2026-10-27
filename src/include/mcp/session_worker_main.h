#pragma once

#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "include/audit_log.h"
#include "include/core.h"
#include "include/mcp/session_worker_protocol.h"

namespace fairyfly::cli {
class CommandHandler;
}

namespace fairyfly::mcp {

/// The SAP session a worker serves once its first call has been admitted.
struct WorkerBinding {
    int connection = -1;
    std::string identity;   ///< the session identity the broker resolved
    std::string owner;      ///< SID/CLIENT/USER
};

/// Before SAP is asked: a worker bound to another session or owner (WORKER_SESSION_MISMATCH), selection input under
/// FAIRYFLY_READ_ONLY (READ_ONLY). nullopt: go on.
std::optional<Result> worker_binding_refusal(const WorkerCall& call, const std::optional<WorkerBinding>& bound,
                                             bool read_only_cap);
/// With the session's live facts: not the call's session and owner (OWNER_SESSION_UNAVAILABLE), or a screen guard the
/// live dynpro no longer matches (SCREEN_CHANGED). nullopt: run it.
std::optional<Result> worker_live_refusal(const WorkerCall& call, const audit::SapFacts& live);

/// One call's settings on the worker's handler: read-only (lifted for an authorized selection-input fill unless
/// FAIRYFLY_READ_ONLY), selection input and the expected MCP session. Put back (read-only, nothing expected) when it
/// ends, also by exception, so nothing leaks into the next call.
class WorkerCallFlags {
public:
    WorkerCallFlags(cli::CommandHandler& handler, const WorkerCall& call, bool read_only_cap);
    ~WorkerCallFlags();
    WorkerCallFlags(const WorkerCallFlags&) = delete;
    WorkerCallFlags& operator=(const WorkerCallFlags&) = delete;

private:
    cli::CommandHandler& handler_;
};

/// The worker's call handling: probes, enumeration and acting calls on the handler `make_handler` creates on first use
/// (SAP GUI's in production). An acting call binds the worker to its session; later calls must match.
WorkerInvoker make_session_worker_invoker(std::function<std::unique_ptr<cli::CommandHandler>()> make_handler);

/// Internal stdio worker entrypoint. The broker starts it with private inherited
/// pipes in the tray user's interactive Windows session.
int run_session_worker_stdio();

} // namespace fairyfly::mcp
