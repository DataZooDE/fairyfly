#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "include/mcp/dispatcher.h"
#include "include/mcp/session_route_coordinator.h"

namespace fairyfly::mcp {

class McpHttpServer;

/// Converts a private worker's Result JSON. Unrecognised or incomplete frames
/// are withheld because a command may already have changed SAP state.
Result result_from_worker_json(const json& reply);

struct SessionWorkerProviderConfig {
    Policy policy;
    int connection = -1;
    std::string session_identity;
    std::vector<ToolSpec> specs;
    std::function<json(int)> probe;
    std::function<json(const WorkerCall&, const std::function<void()>& before_send)> invoke;
    AuditHook audit;
    std::function<void(int, const std::string&)> on_disconnect;
    std::shared_ptr<SessionLeases> leases;
    std::shared_ptr<SessionRoutingState> routing;
    std::shared_ptr<SessionPolicyState> policy_state;
    std::shared_ptr<KeyedRateLimiter> rate_limiter;
};

/// Creates a COM-free dispatcher for one endpoint session lane. Its callbacks
/// must use the same private worker process for probe and action.
std::shared_ptr<CommandDispatcher> make_session_worker_provider(SessionWorkerProviderConfig config);

struct SessionWorkerRuntimeConfig {
    Policy policy;
    std::vector<ToolSpec> specs;
    AuditHook audit;
    std::shared_ptr<SessionLeases> leases;
    std::shared_ptr<SessionRoutingState> routing;
    std::shared_ptr<SessionPolicyState> policy_state;
    std::shared_ptr<KeyedRateLimiter> rate_limiter;
    std::function<json(std::optional<int>)> probe;
    std::function<json(const WorkerCall&, const std::function<void()>& before_send)> invoke;
    std::function<void(int, const std::string&)> retire_connection;
    /// Filled during routing setup; the tray handler calls it immediately
    /// after writing a saved connection during attach or launch.
    std::shared_ptr<std::function<void(int)>> on_connection_changed;
    std::shared_ptr<std::function<std::shared_ptr<void>(const std::string&,
                                                     const std::function<bool()>&)>> on_login_lane_reserve;
    std::shared_ptr<std::shared_timed_mutex> control_probe_gate;
    std::function<void()> shutdown;
    std::size_t max_sessions = 8;
    std::size_t max_queue = 16;
    int call_timeout_ms = 120000;
};

/// Installs owner-aware routing, per-session FIFO lanes, immediate mode
/// invalidation, and worker interruption on a tray HTTP server before bind.
void configure_session_worker_routing(McpHttpServer& server, SessionWorkerRuntimeConfig config);

} // namespace fairyfly::mcp
