#include <catch2/catch_test_macros.hpp>

#include <atomic>
#include <future>
#include <shared_mutex>
#include <thread>

#include "include/mcp/session_route_coordinator.h"
#include "include/mcp/tool_catalog.h"

using namespace fairyfly::mcp;

namespace {
class EmptyProvider final : public ToolProvider {
public:
    std::vector<ToolDef> list_tools() const override { return {}; }
    ToolResult call_tool(const std::string&, const json&, const CallContext&) override { return {}; }
};
}

TEST_CASE("worker probe facts are parsed without trusting incomplete replies", "[mcp][route-coordinator]") {
    json reply = {{"status", "success"}, {"data", {
        {"connection_id", 7}, {"session_identity", "/app/con[0]/ses[0]|key|generation"},
        {"system", "A4H"}, {"client", "001"}, {"user", "ALICE"},
        {"connection_name", "A4H Logon"}}}};
    const auto facts = facts_from_worker_probe(reply);
    CHECK(facts.sap.connection_id == 7);
    CHECK(facts.sap.session_identity == "/app/con[0]/ses[0]|key|generation");
    CHECK(facts.connection_name == "A4H Logon");
    reply["data"]["connection_id"] = "7";
    CHECK_FALSE(facts_from_worker_probe(reply).sap.connection_id);
    reply = {{"status", "error"}, {"error", {{"code", "OWNER_SESSION_UNAVAILABLE"}}}};
    CHECK_FALSE(facts_from_worker_probe(reply).sap.connection_id);
}

TEST_CASE("session route preflight runs on its admission STA and pins a live owner session", "[mcp][route-coordinator]") {
    SessionExecutorPool admission(8, 8, 5000);
    const auto caller_thread = std::this_thread::get_id();
    std::atomic<bool> off_http_thread{false};
    std::atomic<bool> correct_connection{false};
    auto routing = std::make_shared<SessionRoutingState>();
    Policy policy;
    policy.owner_sap_identities = {"A4H/001/ALICE"};
    std::atomic<int> lookups{0};
    std::atomic<int> factories{0};
    SessionRouteCoordinator coordinator(admission, policy, routing, all_tool_specs(),
        [&](std::optional<int> connection) {
            off_http_thread = std::this_thread::get_id() != caller_thread;
            correct_connection = connection == 7;
            ++lookups;
            SessionRouteFacts result;
            result.sap.system = "A4H";
            result.sap.client = "001";
            result.sap.user = "ALICE";
            result.sap.connection_id = 7;
            result.sap.session_identity = "sid|key|generation";
            result.connection_name = "A4H Logon";
            return result;
        },
        [&](const std::string&, int) {
            ++factories;
            return std::make_shared<EmptyProvider>();
        });
    Principal caller;
    caller.id = "token-1";
    caller.sap_systems = {"A4H/001"};
    caller.connections = {"A4H*"};
    const auto first = coordinator.route(caller, "gui_screen_read", {{"connection", 7}});
    CHECK(first.handled);
    CHECK(first.error_code.empty());
    CHECK(first.identity == "sid|key|generation");
    CHECK(first.lane_key == "window:sid");
    CHECK(first.provider);
    const auto second = coordinator.route(caller, "gui_screen_read", {{"connection", 7}});
    CHECK(second.provider == first.provider);
    CHECK(lookups == 2);
    CHECK(factories == 1);
    coordinator.invalidate_providers();
    const auto after_mode_change = coordinator.route(caller, "gui_screen_read", {{"connection", 7}});
    CHECK(after_mode_change.provider != first.provider);
    CHECK(factories == 2);
    const int reads_before_write = lookups;
    CHECK(coordinator.route(caller, "gui_element_fill", {{"connection", 7}}).error_code == "TOOL_UNAVAILABLE_READ_ONLY");
    CHECK(lookups == reads_before_write);
    coordinator.set_read_only(false);
    CHECK(coordinator.route(caller, "gui_element_fill", {{"connection", 7}}).error_code.empty());
    CHECK(off_http_thread);
    CHECK(correct_connection);
}

TEST_CASE("multi-user endpoint requires a token grant for the live SAP identity", "[mcp][route-coordinator][sap-identity]") {
    SessionExecutorPool admission(2, 8, 5000);
    auto routing = std::make_shared<SessionRoutingState>();
    Policy policy;
    policy.owner_sap_identities = {"A4H/001/ALICE", "A4H/001/BOB"};
    std::string live_user = "ALICE";
    SessionRouteCoordinator coordinator(admission, policy, routing, all_tool_specs(),
        [&](std::optional<int>) {
            SessionRouteFacts facts;
            facts.sap.connection_id = 7;
            facts.sap.session_identity = "sid|key|generation";
            facts.sap.system = "A4H";
            facts.sap.client = "001";
            facts.sap.user = live_user;
            facts.connection_name = "Bigfox";
            return facts;
        },
        [](const std::string&, int) { return std::make_shared<EmptyProvider>(); });
    Principal alice;
    alice.id = "alice-token";
    CHECK(coordinator.route(alice, "gui_screen_read", {{"connection", 7}}).error_code == "OWNER_SESSION_UNAVAILABLE");
    alice.sap_identities = {"A4H/001/ALICE"};
    CHECK(coordinator.route(alice, "gui_screen_read", {{"connection", 7}}).error_code.empty());
    live_user = "BOB";
    CHECK(coordinator.route(alice, "gui_screen_read", {{"connection", 7}}).error_code == "OWNER_SESSION_UNAVAILABLE");
    alice.sap_identities = {"A4H/001/BOB"};
    CHECK(coordinator.route(alice, "gui_screen_read", {{"connection", 7}}).error_code.empty());
}

TEST_CASE("session route probe waits while control owns the GUI probe gate", "[mcp][route-coordinator]") {
    SessionExecutorPool admission(2, 2, 5000);
    auto routing = std::make_shared<SessionRoutingState>();
    auto probe_gate = std::make_shared<std::shared_timed_mutex>();
    Policy policy;
    policy.owner_sap_identities = {"A4H/001/ALICE"};
    std::atomic<int> probes{0};
    SessionRouteCoordinator coordinator(admission, policy, routing, all_tool_specs(),
        [&](std::optional<int>) {
            ++probes;
            SessionRouteFacts facts;
            facts.sap.connection_id = 7;
            facts.sap.session_identity = "sid|key|generation";
            facts.sap.system = "A4H";
            facts.sap.client = "001";
            facts.sap.user = "ALICE";
            return facts;
        },
        [](const std::string&, int) { return std::make_shared<EmptyProvider>(); },
        std::chrono::seconds(5), probe_gate);
    std::unique_lock<std::shared_timed_mutex> control(*probe_gate);
    Principal caller;
    auto routed = std::async(std::launch::async, [&] {
        return coordinator.route(caller, "gui_screen_read", {{"connection", 7}});
    });
    CHECK(routed.wait_for(std::chrono::milliseconds(50)) == std::future_status::timeout);
    CHECK(probes == 0);
    control.unlock();
    REQUIRE(routed.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    CHECK(routed.get().error_code.empty());
    CHECK(probes == 1);
}

TEST_CASE("session route denies a changed sticky target and another SAP owner", "[mcp][route-coordinator]") {
    SessionExecutorPool admission(8, 8, 5000);
    auto routing = std::make_shared<SessionRoutingState>();
    routing->set_sticky_connection("token-1", 7, "old-session");
    Policy policy;
    policy.owner_sap_identities = {"A4H/001/ALICE"};
    std::atomic<int> factories{0};
    std::atomic<bool> other_owner{false};
    SessionRouteCoordinator coordinator(admission, policy, routing, all_tool_specs(),
        [&](std::optional<int>) {
            SessionRouteFacts result;
            result.sap.system = "A4H";
            result.sap.client = "001";
            result.sap.user = other_owner ? "BOB" : "ALICE";
            result.sap.connection_id = 7;
            result.sap.session_identity = "new-session";
            return result;
        },
        [&](const std::string&, int) { ++factories; return std::make_shared<EmptyProvider>(); });
    Principal caller;
    caller.id = "token-1";
    const auto route = coordinator.route(caller, "gui_screen_read", json::object());
    CHECK(route.handled);
    CHECK(route.error_code == "OWNER_SESSION_UNAVAILABLE");
    CHECK_FALSE(route.provider);
    routing->set_sticky_connection("token-1", 7, "new-session");
    other_owner = true;
    const auto forbidden_owner = coordinator.route(caller, "gui_screen_read", json::object());
    CHECK(forbidden_owner.error_code == "OWNER_SESSION_UNAVAILABLE");
    other_owner = false;
    caller.sap_systems = {"ZZZ/001"};
    const auto forbidden_system = coordinator.route(caller, "gui_screen_read", json::object());
    CHECK(forbidden_system.error_code == "OWNER_SESSION_UNAVAILABLE");
    CHECK(factories == 0);
}

TEST_CASE("retiring a disconnected connection clears its admission facts", "[mcp][route-coordinator]") {
    SessionExecutorPool admission(2, 8, 5000);
    auto routing = std::make_shared<SessionRoutingState>();
    Policy policy;
    policy.owner_sap_identities = {"A4H/001/ALICE"};
    std::string identity = "old|key|generation";
    int lookups = 0;
    SessionRouteCoordinator coordinator(admission, policy, routing, all_tool_specs(),
        [&](std::optional<int>) {
            ++lookups;
            SessionRouteFacts facts;
            facts.sap.connection_id = 7;
            facts.sap.session_identity = identity;
            facts.sap.system = "A4H";
            facts.sap.client = "001";
            facts.sap.user = "ALICE";
            return facts;
        },
        [](const std::string&, int) { return std::make_shared<EmptyProvider>(); });
    Principal caller;
    CHECK(coordinator.route_cached(caller, "gui_screen_read", {{"connection", 7}}).identity == identity);
    identity = "new|key|generation";
    CHECK(coordinator.route_cached(caller, "gui_screen_read", {{"connection", 7}}).identity == "old|key|generation");
    CHECK(lookups == 1);
    coordinator.invalidate_connection(7, "old|key|generation");
    CHECK(coordinator.route_cached(caller, "gui_screen_read", {{"connection", 7}}).identity == identity);
    CHECK(lookups == 2);
}

TEST_CASE("retiring a disconnected connection also clears automatic selection", "[mcp][route-coordinator]") {
    SessionExecutorPool admission(2, 8, 5000);
    auto routing = std::make_shared<SessionRoutingState>();
    Policy policy;
    policy.owner_sap_identities = {"A4H/001/ALICE"};
    int selected = 7;
    int lookups = 0;
    SessionRouteCoordinator coordinator(admission, policy, routing, all_tool_specs(),
        [&](std::optional<int>) {
            ++lookups;
            SessionRouteFacts facts;
            facts.sap.connection_id = selected;
            facts.sap.session_identity = selected == 7 ? "old|key|generation" : "new|key|generation";
            facts.sap.system = "A4H";
            facts.sap.client = "001";
            facts.sap.user = "ALICE";
            return facts;
        },
        [](const std::string&, int) { return std::make_shared<EmptyProvider>(); });
    Principal caller;
    CHECK(coordinator.route_cached(caller, "gui_screen_read", json::object()).identity == "old|key|generation");
    selected = 8;
    CHECK(coordinator.route_cached(caller, "gui_screen_read", json::object()).identity == "old|key|generation");
    coordinator.invalidate_connection(7, "old|key|generation");
    CHECK(coordinator.route_cached(caller, "gui_screen_read", json::object()).identity == "new|key|generation");
    CHECK(lookups == 2);
}

TEST_CASE("saved connection replacement clears explicit and automatic route facts", "[mcp][route-coordinator]") {
    SessionExecutorPool admission(2, 8, 5000);
    auto routing = std::make_shared<SessionRoutingState>();
    Policy policy;
    policy.owner_sap_identities = {"A4H/001/ALICE"};
    std::string identity = "old|key|generation";
    int lookups = 0;
    SessionRouteCoordinator coordinator(admission, policy, routing, all_tool_specs(),
        [&](std::optional<int>) {
            ++lookups;
            SessionRouteFacts facts;
            facts.sap.connection_id = 7;
            facts.sap.session_identity = identity;
            facts.sap.system = "A4H";
            facts.sap.client = "001";
            facts.sap.user = "ALICE";
            return facts;
        },
        [](const std::string&, int) { return std::make_shared<EmptyProvider>(); });
    Principal caller;
    CHECK(coordinator.route_cached(caller, "gui_screen_read", {{"connection", 7}}).identity == identity);
    CHECK(coordinator.route_cached(caller, "gui_screen_read", json::object()).identity == identity);
    identity = "new|key|generation";
    coordinator.invalidate_saved_connection(7);
    CHECK(coordinator.route_cached(caller, "gui_screen_read", {{"connection", 7}}).identity == identity);
    CHECK(coordinator.route_cached(caller, "gui_screen_read", json::object()).identity == identity);
    CHECK(lookups == 4);
}

TEST_CASE("adding a saved connection clears single-session automatic routing", "[mcp][route-coordinator]") {
    SessionExecutorPool admission(2, 8, 5000);
    auto routing = std::make_shared<SessionRoutingState>();
    Policy policy;
    policy.owner_sap_identities = {"A4H/001/ALICE"};
    bool ambiguous = false;
    int lookups = 0;
    SessionRouteCoordinator coordinator(admission, policy, routing, all_tool_specs(),
        [&](std::optional<int> requested) {
            ++lookups;
            SessionRouteFacts facts;
            if (ambiguous && !requested) return facts;
            facts.sap.connection_id = 7;
            facts.sap.session_identity = "old|key|generation";
            facts.sap.system = "A4H";
            facts.sap.client = "001";
            facts.sap.user = "ALICE";
            return facts;
        },
        [](const std::string&, int) { return std::make_shared<EmptyProvider>(); });
    Principal caller;
    CHECK(coordinator.route_cached(caller, "gui_screen_read", json::object()).identity == "old|key|generation");
    ambiguous = true; // attach created another saved connection, ID 8
    coordinator.invalidate_saved_connection(8);
    CHECK(coordinator.route_cached(caller, "gui_screen_read", json::object()).error_code == "OWNER_SESSION_UNAVAILABLE");
    CHECK(lookups == 2);
}

TEST_CASE("session route requires owner policy and a complete stable identity", "[mcp][route-coordinator]") {
    SessionExecutorPool admission(8, 8, 5000);
    auto routing = std::make_shared<SessionRoutingState>();
    std::atomic<int> lookups{0};
    std::atomic<int> factories{0};
    std::string identity = "sid||generation";
    auto lookup = [&](std::optional<int>) {
        ++lookups;
        SessionRouteFacts result;
        result.sap.system = "A4H";
        result.sap.client = "001";
        result.sap.user = "ALICE";
        result.sap.connection_id = 7;
        result.sap.session_identity = identity;
        return result;
    };
    auto factory = [&](const std::string&, int) {
        ++factories;
        return std::make_shared<EmptyProvider>();
    };
    Principal caller;
    Policy unrestricted;
    SessionRouteCoordinator no_owner(admission, unrestricted, routing, all_tool_specs(), lookup, factory);
    CHECK(no_owner.route(caller, "gui_screen_read", {{"connection", 7}}).error_code == "OWNER_SESSION_UNAVAILABLE");
    CHECK(lookups == 0);
    Policy restricted;
    restricted.owner_sap_identities = {"A4H/001/ALICE"};
    SessionRouteCoordinator with_owner(admission, restricted, routing, all_tool_specs(), lookup, factory);
    CHECK(with_owner.route(caller, "gui_screen_read", {{"connection", 7}}).error_code == "OWNER_SESSION_UNAVAILABLE");
    identity = "sid|key|";
    CHECK(with_owner.route(caller, "gui_screen_read", {{"connection", 7}}).error_code == "OWNER_SESSION_UNAVAILABLE");
    CHECK(factories == 0);
}

TEST_CASE("a held admission for one connection does not delay another", "[mcp][route-coordinator][parallel]") {
    using namespace std::chrono_literals;
    SessionExecutorPool admission(2, 2, 5000);
    auto routing = std::make_shared<SessionRoutingState>();
    Policy policy;
    policy.owner_sap_identities = {"A4H/001/ALICE"};
    std::promise<void> entered;
    std::promise<void> release;
    auto released = release.get_future().share();
    SessionRouteCoordinator coordinator(admission, policy, routing, all_tool_specs(),
        [&](std::optional<int> connection) {
            if (connection == 1) { entered.set_value(); released.wait(); }
            SessionRouteFacts result;
            result.sap.system = "A4H";
            result.sap.client = "001";
            result.sap.user = "ALICE";
            result.sap.connection_id = connection;
            result.sap.session_identity = "sid" + std::to_string(*connection) + "|key|generation";
            return result;
        },
        [](const std::string&, int) { return std::make_shared<EmptyProvider>(); });
    Principal caller;
    auto slow = std::async(std::launch::async, [&] {
        return coordinator.route(caller, "gui_screen_read", {{"connection", 1}});
    });
    CHECK(entered.get_future().wait_for(1s) == std::future_status::ready);
    auto fast = std::async(std::launch::async, [&] {
        return coordinator.route(caller, "gui_screen_read", {{"connection", 2}});
    });
    CHECK(fast.wait_for(500ms) == std::future_status::ready);
    CHECK(fast.get().identity == "sid2|key|generation");
    release.set_value();
    CHECK(slow.get().identity == "sid1|key|generation");
}

TEST_CASE("session route bounds worker probes before SAP admission", "[mcp][route-coordinator][rate]") {
    SessionExecutorPool admission(1, 4, 5000);
    auto routing = std::make_shared<SessionRoutingState>();
    Policy policy;
    policy.owner_sap_identities = {"A4H/001/ALICE"};
    policy.max_calls_per_minute = 1;
    int probes = 0;
    SessionRouteCoordinator coordinator(admission, policy, routing, all_tool_specs(),
        [&](std::optional<int>) {
            ++probes;
            SessionRouteFacts result;
            result.sap.connection_id = 7;
            result.sap.session_identity = "sid|key|generation";
            result.sap.system = "A4H";
            result.sap.client = "001";
            result.sap.user = "ALICE";
            return result;
        },
        [](const std::string&, int) { return std::make_shared<EmptyProvider>(); });
    Principal caller;
    caller.id = "token-1";
    CHECK(coordinator.route(caller, "gui_screen_read", {{"connection", 7}}).error_code.empty());
    CHECK(coordinator.route(caller, "gui_screen_read", {{"connection", 7}}).error_code.empty());
    CHECK(coordinator.route(caller, "gui_screen_read", {{"connection", 7}}).error_code == "SESSION_ROUTE_RATE_LIMITED");
    CHECK(probes == 2);
}

TEST_CASE("cached route admits behind a busy same-session worker, then fresh route still probes", "[mcp][route-coordinator][parallel]") {
    SessionExecutorPool admission(1, 2, 5000);
    auto routing = std::make_shared<SessionRoutingState>();
    Policy policy;
    policy.owner_sap_identities = {"A4H/001/ALICE"};
    int probes = 0;
    SessionRouteCoordinator coordinator(admission, policy, routing, all_tool_specs(),
        [&](std::optional<int>) {
            ++probes;
            SessionRouteFacts result;
            result.sap.connection_id = 7;
            result.sap.session_identity = "sid|key|generation";
            result.sap.system = "A4H";
            result.sap.client = "001";
            result.sap.user = "ALICE";
            return result;
        },
        [](const std::string&, int) { return std::make_shared<EmptyProvider>(); });
    Principal caller;
    caller.id = "token-1";
    REQUIRE(coordinator.route(caller, "gui_screen_read", {{"connection", 7}}).error_code.empty());
    CHECK(coordinator.route_cached(caller, "gui_screen_read", {{"connection", 7}}).error_code.empty());
    CHECK(probes == 1);
    CHECK(coordinator.route(caller, "gui_screen_read", {{"connection", 7}}).error_code.empty());
    CHECK(probes == 2);
}
