#include "include/mcp/session_route_coordinator.h"

#include <algorithm>
#include <future>
#include <utility>

#include "include/auth/authorize.h"
#include "include/mcp/policy.h"
#include "include/mcp/session_lease_policy.h"

namespace fairyfly::mcp {
namespace {

HttpEndpoint::SessionRoute denied(const std::string& code) {
    HttpEndpoint::SessionRoute route;
    route.handled = true;
    route.error_code = code;
    route.error_message = "the requested SAP session is unavailable";
    return route;
}

bool complete_identity(const std::string& identity) {
    const auto first = identity.find('|');
    if (first == std::string::npos || first == 0) return false;
    const auto second = identity.find('|', first + 1);
    return second != std::string::npos && second > first + 1 &&
           second + 1 < identity.size() && identity.find('|', second + 1) == std::string::npos;
}

} // namespace

SessionRouteFacts facts_from_worker_probe(const json& result) {
    SessionRouteFacts facts;
    if (!result.is_object() || result.value("status", std::string()) != "success" ||
        !result.contains("data") || !result["data"].is_object()) return facts;
    const auto& data = result["data"];
    for (const char* field : {"session_identity", "system", "client", "user", "connection_name"})
        if (!data.contains(field) || !data[field].is_string()) return {};
    if (!data.contains("connection_id") || !data["connection_id"].is_number_integer() ||
        data["connection_id"] < 0 || data["connection_id"] > 1000000) return {};
    facts.sap.connection_id = data["connection_id"].get<int>();
    facts.sap.session_identity = data["session_identity"].get<std::string>();
    facts.sap.system = data["system"].get<std::string>();
    facts.sap.client = data["client"].get<std::string>();
    facts.sap.user = data["user"].get<std::string>();
    facts.connection_name = data["connection_name"].get<std::string>();
    if (data.contains("transaction") && data["transaction"].is_string())
        facts.sap.transaction = data["transaction"].get<std::string>();
    if (data.contains("program") && data["program"].is_string())
        facts.sap.program = data["program"].get<std::string>();
    if (data.contains("screen_number") && data["screen_number"].is_string())
        facts.sap.screen_number = data["screen_number"].get<std::string>();
    return facts;
}

SessionRouteCoordinator::SessionRouteCoordinator(SessionExecutorPool& admission_pool, Policy policy,
                                                 std::shared_ptr<SessionRoutingState> routing,
                                                 std::vector<ToolSpec> specs, FactsLookup lookup,
                                                 ProviderFactory provider_factory,
                                                 std::chrono::milliseconds timeout,
                                                 std::shared_ptr<std::shared_timed_mutex> probe_gate)
    : admission_pool_(admission_pool), policy_(std::move(policy)), read_only_(policy_.read_only),
      routing_(std::move(routing)),
      specs_(std::move(specs)), lookup_(std::move(lookup)),
      provider_factory_(std::move(provider_factory)), timeout_(timeout), probe_gate_(std::move(probe_gate)) {}

HttpEndpoint::SessionRoute SessionRouteCoordinator::route(const Principal& principal,
                                                           const std::string& tool, const json& args) {
    return route_impl(principal, tool, args, false);
}

HttpEndpoint::SessionRoute SessionRouteCoordinator::route_cached(const Principal& principal,
                                                                  const std::string& tool, const json& args) {
    return route_impl(principal, tool, args, true);
}

HttpEndpoint::SessionRoute SessionRouteCoordinator::route_impl(const Principal& principal,
                                                                const std::string& tool, const json& args,
                                                                bool allow_cached) {
    if (!routing_ || !lookup_ || !provider_factory_) return denied("SESSION_ROUTE_UNAVAILABLE");
    if (policy_.owner_sap_identities.empty()) return denied("OWNER_SESSION_UNAVAILABLE");
    const SessionRoutePlan plan = plan_session_route(principal, tool, args, policy_, *routing_, specs_);
    if (plan.kind == SessionRoutePlan::Kind::GlobalControl) {
        HttpEndpoint::SessionRoute route;
        route.global_control = true;
        return route;
    }
    if (plan.kind == SessionRoutePlan::Kind::Refused) return denied(plan.error_code);
    const auto spec = std::find_if(specs_.begin(), specs_.end(), [&](const ToolSpec& entry) {
        return entry.def.name == tool;
    });
    if (spec == specs_.end() || !auth::scope_allows(principal, spec->family, tool))
        return denied("OWNER_SESSION_UNAVAILABLE");
    Policy current_policy = policy_;
    current_policy.read_only = read_only_.load();
    current_policy.allow_write = !current_policy.read_only;
    const bool selection_input = auth::selection_input_path(principal, tool, current_policy);
    if (!selection_input) {
        const PolicyDecision allowed = check_call(*spec, args, current_policy);
        if (!allowed.allowed) return denied(allowed.code.empty() ? "READ_ONLY" : allowed.code);
        if (principal.read_only && spec->write_tool) return denied("READ_ONLY");
    }

    // Admission and the dequeue route recheck each probe SAP. Keep a separate
    // bounded budget so rejected or over-limit calls cannot monopolise worker
    // processes before the dispatcher's ordinary call budget is consumed.
    const int configured = principal.rate_per_minute > 0 ? principal.rate_per_minute : policy_.max_calls_per_minute;
    const int probe_budget = configured <= 0 || configured > 60 ? 120 : std::max(2, configured * 2);
    if (!admission_limiter_.allow(principal_key(principal), probe_budget, std::chrono::steady_clock::now()))
        return denied("SESSION_ROUTE_RATE_LIMITED");

    SessionRouteFacts facts;
    bool from_cache = false;
    const int cache_key = plan.connection.value_or(-1);
    std::uint64_t facts_epoch = 0;
    {
        std::lock_guard<std::mutex> lock(facts_mu_);
        facts_epoch = facts_epoch_[cache_key];
    }
    if (allow_cached) {
        std::lock_guard<std::mutex> lock(facts_mu_);
        if (const auto found = facts_cache_.find(cache_key); found != facts_cache_.end()) {
            facts = found->second;
            from_cache = true;
        }
    }
    if (!from_cache) {
        auto promise = std::make_shared<std::promise<SessionRouteFacts>>();
        auto future = promise->get_future();
        ExecJob preflight;
        preflight.tool = "session route preflight";
        preflight.run = [lookup = lookup_, connection = plan.connection, promise,
                         gate = probe_gate_](CallState& state) -> json {
            try {
                std::shared_lock<std::shared_timed_mutex> probe_lock;
                if (gate) probe_lock = std::shared_lock<std::shared_timed_mutex>(*gate);
                promise->set_value(state.cancelled ? SessionRouteFacts{} : lookup(connection));
            }
            catch (...) { promise->set_exception(std::current_exception()); }
            return json();
        };
        std::shared_ptr<CallState> state;
        const std::string admission_key = plan.connection ? "connection:" + std::to_string(*plan.connection)
                                                          : "connection:auto";
        if (admission_pool_.submit(admission_key, std::move(preflight), &state) != SubmitResult::Queued)
            return denied("SESSION_ROUTE_UNAVAILABLE");
        if (future.wait_for(timeout_) != std::future_status::ready) {
            admission_pool_.cancel(admission_key, state);
            return denied("SESSION_ROUTE_UNAVAILABLE");
        }
        try { facts = future.get(); }
        catch (...) { return denied("OWNER_SESSION_UNAVAILABLE"); }
    }

    const auto& sap = facts.sap;
    if (!sap.connection_id || (plan.connection && sap.connection_id != plan.connection) ||
        !complete_identity(sap.session_identity) || sap.system.empty() || sap.client.empty() || sap.user.empty() ||
        (!plan.sticky_identity.empty() && plan.sticky_identity != sap.session_identity))
        return denied("OWNER_SESSION_UNAVAILABLE");
    const std::string owner = sap.system + "/" + sap.client + "/" + sap.user;
    if (!owner_identity_allowed(policy_, principal, owner))
        return denied("OWNER_SESSION_UNAVAILABLE");
    const std::string system = sap.system + "/" + sap.client;
    if (!principal.sap_systems.empty() && !auth::system_allowed(principal.sap_systems, system))
        return denied("OWNER_SESSION_UNAVAILABLE");
    auth::SessionTarget target;
    target.system = system;
    target.user = sap.user;
    target.connection_name = facts.connection_name;
    const std::string auth_tool = tool == "gui_batch" ? "gui_screen_read" : tool;
    if (!auth::authorize_session_target(principal, auth_tool, args, target).allowed)
        return denied("OWNER_SESSION_UNAVAILABLE");
    if (!from_cache) {
        std::lock_guard<std::mutex> lock(facts_mu_);
        if (facts_epoch_[cache_key] == facts_epoch) {
            if (facts_cache_.size() >= 64 && !facts_cache_.count(cache_key)) facts_cache_.erase(facts_cache_.begin());
            facts_cache_[cache_key] = facts;
        }
    }

    std::shared_ptr<ToolProvider> provider;
    const auto key = std::make_pair(sap.session_identity, *sap.connection_id);
    {
        std::lock_guard<std::mutex> lock(providers_mu_);
        if (const auto it = providers_.find(key); it != providers_.end()) provider = it->second.lock();
        if (!provider) {
            try { provider = provider_factory_(key.first, key.second); }
            catch (...) { return denied("SESSION_ROUTE_UNAVAILABLE"); }
            if (!provider) return denied("SESSION_ROUTE_UNAVAILABLE");
            if (providers_.size() > 64) {
                for (auto it = providers_.begin(); it != providers_.end();) {
                    if (it->second.expired()) it = providers_.erase(it);
                    else ++it;
                }
            }
            providers_[key] = provider;
        }
    }
    HttpEndpoint::SessionRoute route;
    route.handled = true;
    route.identity = sap.session_identity;
    route.lane_key = window_lane_key(sap.session_identity);
    route.provider = std::move(provider);
    return route;
}

void SessionRouteCoordinator::invalidate_providers() {
    std::lock_guard<std::mutex> lock(providers_mu_);
    providers_.clear();
}

void SessionRouteCoordinator::invalidate_connection(int connection, const std::string& identity) {
    {
        std::lock_guard<std::mutex> lock(facts_mu_);
        for (const int key : {connection, -1}) {
            ++facts_epoch_[key];
            if (const auto it = facts_cache_.find(key); it != facts_cache_.end() &&
                it->second.sap.connection_id == connection &&
                it->second.sap.session_identity == identity)
                facts_cache_.erase(it);
        }
    }
    {
        std::lock_guard<std::mutex> lock(providers_mu_);
        providers_.erase({identity, connection});
    }
}

void SessionRouteCoordinator::invalidate_saved_connection(int connection) {
    {
        std::lock_guard<std::mutex> lock(facts_mu_);
        ++facts_epoch_[connection];
        facts_cache_.erase(connection);
        ++facts_epoch_[-1];
        facts_cache_.erase(-1);
    }
    {
        std::lock_guard<std::mutex> lock(providers_mu_);
        for (auto it = providers_.begin(); it != providers_.end();) {
            if (it->first.second == connection) it = providers_.erase(it);
            else ++it;
        }
    }
}

void SessionRouteCoordinator::set_read_only(bool read_only) {
    read_only_.store(read_only);
    invalidate_providers();
}

} // namespace fairyfly::mcp
