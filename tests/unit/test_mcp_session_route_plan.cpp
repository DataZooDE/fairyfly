#include <catch2/catch_test_macros.hpp>

#include "include/mcp/session_route_plan.h"
#include "include/mcp/tool_catalog.h"

using namespace fairyfly::mcp;

TEST_CASE("session route planning follows dispatcher connection precedence", "[mcp][route-plan]") {
    Principal caller;
    caller.id = "token-A";
    SessionRoutingState routing;
    routing.set_sticky_connection(caller.id, 3, "old-session");
    Policy policy;
    policy.default_connection = 5;
    const auto specs = all_tool_specs();

    const auto explicit_target = plan_session_route(caller, "gui_screen_read", {{"connection", 7}}, policy, routing, specs);
    CHECK(explicit_target.kind == SessionRoutePlan::Kind::Session);
    CHECK(explicit_target.connection == 7);
    CHECK(explicit_target.sticky_identity.empty());

    const auto default_target = plan_session_route(caller, "gui_screen_read", json::object(), policy, routing, specs);
    CHECK(default_target.connection == 5);
    CHECK(default_target.sticky_identity.empty());

    policy.default_connection.reset();
    const auto sticky_target = plan_session_route(caller, "gui_screen_read", json::object(), policy, routing, specs);
    CHECK(sticky_target.connection == 3);
    CHECK(sticky_target.sticky_identity == "old-session");
}

TEST_CASE("session route planning keeps control calls and batches safe", "[mcp][route-plan]") {
    Principal caller;
    Policy policy;
    SessionRoutingState routing;
    const auto specs = all_tool_specs();
    for (const char* tool : {"gui_session_list", "gui_connection_list", "gui_credentials_list",
                             "gui_doctor", "gui_session_lease"}) {
        CHECK(plan_session_route(caller, tool, json::object(), policy, routing, specs).kind ==
              SessionRoutePlan::Kind::GlobalControl);
    }
    const auto attach = plan_session_route(caller, "gui_session_attach",
        {{"session_id", "/app/con[0]/ses[0]"}}, policy, routing, specs);
    CHECK(attach.kind == SessionRoutePlan::Kind::GlobalControl);
    CHECK(plan_session_route(caller, "gui_session_login", {{"connection", 9}}, policy, routing, specs).kind ==
          SessionRoutePlan::Kind::GlobalControl);
    CHECK(plan_session_route(caller, "gui_session_launch", {{"name", "A4H Logon"}}, policy, routing, specs).kind ==
          SessionRoutePlan::Kind::GlobalControl);
    const auto implicit_attach = plan_session_route(caller, "gui_session_attach", json::object(), policy, routing, specs);
    CHECK(implicit_attach.kind == SessionRoutePlan::Kind::Refused);
    CHECK(implicit_attach.error_code == "SESSION_ID_REQUIRED");
    const auto batch_missing = plan_session_route(caller, "gui_batch", json::object(), policy, routing, specs);
    CHECK(batch_missing.kind == SessionRoutePlan::Kind::Refused);
    CHECK(batch_missing.error_code == "BATCH_SESSION_REQUIRED");
    const auto batch = plan_session_route(caller, "gui_batch", {{"connection", 9}}, policy, routing, specs);
    CHECK(batch.kind == SessionRoutePlan::Kind::Session);
    CHECK(batch.connection == 9);
    const auto disconnect = plan_session_route(caller, "gui_session_disconnect", {{"connection", 9}}, policy, routing, specs);
    CHECK(disconnect.kind == SessionRoutePlan::Kind::Session);
    CHECK(disconnect.connection == 9);
}

TEST_CASE("claimed prelogin close uses the control path", "[mcp][route-plan][prelogin]") {
    Principal owner;
    owner.id = "owner-token";
    Principal other;
    other.id = "other-token";
    Policy policy;
    policy.owner_sap_identities = {"A4H/001/ALICE", "A4H/001/BOB"};
    SessionRoutingState routing;
    REQUIRE(routing.bind_prelogin(owner.id, 9, "window|key|generation"));
    const auto specs = all_tool_specs();
    const json close = {{"connection", 9}, {"close_session", true}};
    CHECK(plan_session_route(owner, "gui_session_disconnect", close, policy, routing, specs).kind ==
          SessionRoutePlan::Kind::GlobalControl);
    CHECK(plan_session_route(other, "gui_session_disconnect", close, policy, routing, specs).kind ==
          SessionRoutePlan::Kind::Session);
    CHECK(plan_session_route(owner, "gui_session_disconnect", {{"connection", 9}}, policy, routing, specs).kind ==
          SessionRoutePlan::Kind::Session);
}

TEST_CASE("every catalog tool has an explicit route classification", "[mcp][route-plan]") {
    Principal caller;
    Policy policy;
    SessionRoutingState routing;
    const auto specs = all_tool_specs();
    for (const auto& spec : specs) {
        const auto plan = plan_session_route(caller, spec.def.name, json::object(), policy, routing, specs);
        INFO(spec.def.name);
        CHECK(plan.error_code != "SESSION_ROUTE_REQUIRED");
    }
}
