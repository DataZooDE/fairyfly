#include "include/mcp/session_worker_provider.h"

#include <stdexcept>
#include <utility>

#include "include/mcp/session_worker_process.h"
#include "include/mcp/http_server.h"
#include "include/mcp/session_route_coordinator.h"

namespace fairyfly::mcp {
namespace {
Result error_result(const std::string& code, const std::string& message) {
    Result result;
    result.status = Result::Status::Error;
    result.error = {{"code", code}, {"message", message}};
    return result;
}
}

Result result_from_worker_json(const json& reply) {
    if (!reply.is_object() || !reply.contains("status") || !reply["status"].is_string())
        return error_result("OUTCOME_UNKNOWN", "invalid session worker result");
    Result result;
    const std::string status = reply["status"].get<std::string>();
    if (status == "success") {
        if (!reply.contains("data"))
            return error_result("OUTCOME_UNKNOWN", "incomplete session worker result");
        result.status = Result::Status::Success;
        result.data = reply["data"];
    } else if (status == "error") {
        if (!reply.contains("error") || !reply["error"].is_object() ||
            !reply["error"].contains("code") || !reply["error"]["code"].is_string() ||
            !reply["error"].contains("message") || !reply["error"]["message"].is_string())
            return error_result("OUTCOME_UNKNOWN", "incomplete session worker error");
        result.status = Result::Status::Error;
        result.error = reply["error"];
    } else {
        return error_result("OUTCOME_UNKNOWN", "unknown session worker result status");
    }
    if (reply.contains("diagnostics")) result.diagnostics = reply["diagnostics"];
    if (reply.contains("metadata") && reply["metadata"].is_object() &&
        reply["metadata"].contains("duration_ms") && reply["metadata"]["duration_ms"].is_number_integer()) {
        const auto duration = reply["metadata"]["duration_ms"].get<long long>();
        if (duration >= 0) result.duration = std::chrono::milliseconds(duration);
    }
    return result;
}

std::shared_ptr<CommandDispatcher> make_session_worker_provider(SessionWorkerProviderConfig config) {
    if (config.connection < 0 || config.session_identity.empty() || !config.probe || !config.invoke ||
        config.policy.owner_sap_identities.empty())
        throw std::invalid_argument("session worker provider requires a bound owner session and callbacks");
    const int connection = config.connection;
    const std::string identity = std::move(config.session_identity);
    auto probe = std::move(config.probe);
    auto invoke = std::move(config.invoke);
    auto live = [probe, connection, identity](std::optional<int> requested) -> SessionRouteFacts {
        if (requested && *requested != connection) return {};
        SessionRouteFacts facts = facts_from_worker_probe(probe(connection));
        if (facts.sap.connection_id != connection || facts.sap.session_identity != identity) return {};
        return facts;
    };
    auto unavailable = [](const std::vector<std::string>&) -> Result {
        return error_result("WORKER_UNSUPPORTED", "the command requires a session worker");
    };
    AuditHook audit = [sink = std::move(config.audit), on_disconnect = std::move(config.on_disconnect)]
        (const McpCallRecord& record) {
        if (record.tool == "gui_session_disconnect" &&
            (record.status == "success" || record.error_code == "OUTCOME_UNKNOWN") &&
            record.connection && record.sap && on_disconnect) {
            try { on_disconnect(*record.connection, record.sap->session_identity); } catch (...) {}
        }
        if (sink) sink(record);
    };
    auto dispatcher = std::make_shared<CommandDispatcher>(unavailable, std::move(config.policy),
                                                           std::move(audit), std::move(config.specs));
    if (config.leases) dispatcher->set_session_leases(std::move(config.leases));
    if (config.routing) dispatcher->set_routing_state(std::move(config.routing));
    if (config.policy_state) dispatcher->set_session_policy_state(std::move(config.policy_state));
    if (config.rate_limiter) dispatcher->set_shared_rate_limiter(std::move(config.rate_limiter));
    dispatcher->set_sap_facts_provider([live](std::optional<int> requested) -> std::optional<audit::SapFacts> {
        const auto facts = live(requested).sap;
        return facts.connection_id ? std::optional<audit::SapFacts>(facts) : std::nullopt;
    });
    dispatcher->set_session_target_resolver([live, connection](const CommandDispatcher::TargetQuery& query) {
        auth::SessionTarget target;
        target.ambiguous = true;
        if (!query.logon_name.empty() || !query.session_id.empty() ||
            (query.connection && *query.connection != connection)) return target;
        const auto facts = live(connection);
        if (!facts.sap.connection_id) return target;
        target.ambiguous = false;
        target.system = facts.sap.system + "/" + facts.sap.client;
        target.user = facts.sap.user;
        target.connection_name = facts.connection_name;
        return target;
    });
    // WorkerCall carries these checked settings; the worker applies them to its
    // private handler. No handler state is changed in the HTTP/tray process.
    dispatcher->set_read_only_override([](bool) {});
    dispatcher->set_selection_input_override([](bool, const std::string&, const std::string&) {});
    dispatcher->set_owner_session_override([](const std::string&, const std::string&) {});
    dispatcher->set_session_invoker_with_gate([invoke = std::move(invoke), connection, identity]
        (const WorkerCall& call, const std::function<void()>& before_send) {
        if (call.connection != connection || call.session_identity != identity)
            return error_result("OWNER_SESSION_UNAVAILABLE", "session worker target changed");
        try { return result_from_worker_json(invoke(call, before_send)); }
        catch (const WorkerTransportError& e) { return error_result(e.code(), e.what()); }
        catch (...) { return error_result("OUTCOME_UNKNOWN", "session worker outcome is unknown"); }
    });
    return dispatcher;
}

void configure_session_worker_routing(McpHttpServer& server, SessionWorkerRuntimeConfig config) {
    if (config.policy.owner_sap_identities.empty() || !config.leases || !config.routing ||
        !config.policy_state || !config.rate_limiter || !config.probe || !config.invoke ||
        config.max_sessions == 0 || config.max_queue == 0 || config.call_timeout_ms <= 0)
        throw std::invalid_argument("owner session routing is incomplete");
    auto shared = std::make_shared<SessionWorkerRuntimeConfig>(std::move(config));
    auto mode = std::make_shared<std::atomic<bool>>(shared->policy.read_only);
    auto generation = std::make_shared<std::atomic<std::uint64_t>>(0);
    auto admission = std::make_shared<SessionExecutorPool>(shared->max_sessions, shared->max_queue,
                                                            shared->call_timeout_ms);
    auto sessions = std::make_shared<SessionExecutorPool>(shared->max_sessions, shared->max_queue,
                                                           shared->call_timeout_ms);
    if (shared->on_login_lane_reserve) {
        *shared->on_login_lane_reserve = [sessions, timeout = shared->call_timeout_ms](
            const std::string& key, const std::function<bool()>& cancelled)
            -> std::shared_ptr<void> {
            return sessions->reserve(key, std::chrono::milliseconds(timeout), cancelled);
        };
    }
    auto coordinator_slot = std::make_shared<std::weak_ptr<SessionRouteCoordinator>>();
    auto provider_factory = [shared, mode, generation, coordinator_slot](const std::string& identity, int connection)
        -> std::shared_ptr<ToolProvider> {
        SessionWorkerProviderConfig provider;
        provider.policy = shared->policy;
        provider.policy.read_only = mode->load();
        const bool provider_read_only = provider.policy.read_only;
        provider.policy.allow_write = !provider.policy.read_only;
        provider.connection = connection;
        provider.session_identity = identity;
        provider.specs = shared->specs;
        provider.audit = shared->audit;
        provider.on_disconnect = [shared, coordinator_slot](int id, const std::string& old_identity) {
            if (const auto coordinator = coordinator_slot->lock())
                coordinator->invalidate_connection(id, old_identity);
            if (shared->retire_connection) shared->retire_connection(id, old_identity);
        };
        provider.leases = shared->leases;
        provider.routing = shared->routing;
        provider.policy_state = shared->policy_state;
        provider.rate_limiter = shared->rate_limiter;
        provider.probe = [shared](int id) { return shared->probe(id); };
        const auto version = generation->load();
        provider.invoke = [shared, mode, generation, version, provider_read_only](const WorkerCall& call,
                                                         const std::function<void()>& before_send) {
            const auto guarded = [&] {
                if (generation->load() != version || mode->load() != provider_read_only)
                    throw WorkerTransportError("SESSION_ROUTE_CHANGED", "session policy changed before worker dispatch");
                before_send();
            };
            return shared->invoke(call, guarded);
        };
        return make_session_worker_provider(std::move(provider));
    };
    auto coordinator = std::make_shared<SessionRouteCoordinator>(*admission, shared->policy,
        shared->routing, shared->specs,
        [shared](std::optional<int> connection) { return facts_from_worker_probe(shared->probe(connection)); },
        std::move(provider_factory), std::chrono::seconds(5), shared->control_probe_gate);
    *coordinator_slot = coordinator;
    if (shared->on_connection_changed) {
        *shared->on_connection_changed = [weak = std::weak_ptr<SessionRouteCoordinator>(coordinator)](int id) {
            if (const auto live = weak.lock()) live->invalidate_saved_connection(id);
        };
    }
    server.set_session_router(sessions,
        [coordinator, admission](const Principal& principal, const std::string& tool, const json& args) {
            return coordinator->route_cached(principal, tool, args);
        },
        [coordinator, admission](const Principal& principal, const std::string& tool, const json& args) {
            return coordinator->route(principal, tool, args);
        });
    server.set_session_mode_hook([mode, generation, coordinator](bool read_only) {
        mode->store(read_only);
        generation->fetch_add(1);
        coordinator->set_read_only(read_only);
    });
    server.set_session_shutdown([shared, admission] {
        if (shared->shutdown) { try { shared->shutdown(); } catch (...) {} }
        admission->request_stop();
    });
}

} // namespace fairyfly::mcp
