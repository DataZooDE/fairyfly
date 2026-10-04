#pragma once

#include <chrono>
#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <utility>

#include "include/audit_log.h"
#include "include/mcp/http_endpoint.h"
#include "include/mcp/policy.h"
#include "include/mcp/session_route_plan.h"

namespace fairyfly::mcp {

struct SessionRouteFacts {
    audit::SapFacts sap;
    std::string connection_name;
};

/// Parses the version-checked private worker Result. Malformed or failed
/// probes return empty facts, which the coordinator refuses.
SessionRouteFacts facts_from_worker_probe(const json& result);

/// Routes HTTP requests using bounded per-connection admission lanes for live
/// facts. The lookup owns COM on its admission thread; the provider factory
/// must create a COM-free, lane-confined provider.
class SessionRouteCoordinator {
public:
    using FactsLookup = std::function<SessionRouteFacts(std::optional<int>)>;
    using ProviderFactory = std::function<std::shared_ptr<ToolProvider>(const std::string&, int)>;

    SessionRouteCoordinator(SessionExecutorPool& admission_pool, Policy policy,
                            std::shared_ptr<SessionRoutingState> routing,
                            std::vector<ToolSpec> specs, FactsLookup lookup,
                            ProviderFactory provider_factory,
                            std::chrono::milliseconds timeout = std::chrono::seconds(5),
                            std::shared_ptr<std::shared_timed_mutex> probe_gate = {});

    HttpEndpoint::SessionRoute route(const Principal& principal, const std::string& tool,
                                     const json& args);
    /// Admission may use a previously verified route while that worker is
    /// busy. The endpoint must call route() again at dequeue for live facts.
    HttpEndpoint::SessionRoute route_cached(const Principal& principal, const std::string& tool,
                                            const json& args);
    /// A mode/policy change replaces providers. Already queued jobs fail their
    /// endpoint route recheck before reaching the old provider.
    void invalidate_providers();
    /// A successful disconnect removes this saved connection. Clear its
    /// admission facts so a later connection using the same numeric ID is probed.
    void invalidate_connection(int connection, const std::string& identity);
    /// A tray control command has created or replaced a saved connection.
    void invalidate_saved_connection(int connection);
    void set_read_only(bool read_only);

private:
    HttpEndpoint::SessionRoute route_impl(const Principal&, const std::string&, const json&, bool allow_cached);
    SessionExecutorPool& admission_pool_;
    Policy policy_;
    std::atomic<bool> read_only_;
    std::shared_ptr<SessionRoutingState> routing_;
    std::vector<ToolSpec> specs_;
    FactsLookup lookup_;
    ProviderFactory provider_factory_;
    std::chrono::milliseconds timeout_;
    std::shared_ptr<std::shared_timed_mutex> probe_gate_;
    KeyedRateLimiter admission_limiter_;
    std::mutex facts_mu_;
    std::map<int, SessionRouteFacts> facts_cache_;
    std::map<int, std::uint64_t> facts_epoch_;
    std::mutex providers_mu_;
    std::map<std::pair<std::string, int>, std::weak_ptr<ToolProvider>> providers_;
};

} // namespace fairyfly::mcp
