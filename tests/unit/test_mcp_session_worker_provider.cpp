#include <catch2/catch_test_macros.hpp>

#include "include/mcp/session_worker_provider.h"
#include "include/mcp/tool_catalog.h"

using namespace fairyfly::mcp;

namespace {
json probe_reply(const std::string& user = "ALICE") {
    return {{"status", "success"}, {"data", {{"connection_id", 7},
        {"session_identity", "/app/con[0]/ses[0]|key|generation"},
        {"system", "A4H"}, {"client", "001"}, {"user", user},
        {"connection_name", "A4H Logon"}}}};
}

CallContext caller() {
    CallContext ctx;
    ctx.http = true;
    ctx.principal.id = "token-1";
    ctx.principal.name = "agent";
    ctx.reauthorize = [] { return true; };
    return ctx;
}
}

TEST_CASE("worker result decoder rejects malformed replies", "[mcp][worker-provider]") {
    CHECK(result_from_worker_json({{"status", "success"}}).error["code"] == "OUTCOME_UNKNOWN");
    CHECK(result_from_worker_json({{"status", "error"}, {"error", {{"code", "SAP_ERROR"},
        {"message", "failed"}}}}).error["code"] == "SAP_ERROR");
    CHECK(result_from_worker_json({{"status", "success"}, {"data", {{"title", "Easy Access"}}}})
              .status == fairyfly::Result::Status::Success);
}

TEST_CASE("routed provider binds worker action to live owner and session", "[mcp][worker-provider]") {
    Policy policy;
    policy.read_only = true;
    policy.owner_sap_identities = {"A4H/001/ALICE"};
    int probes = 0, actions = 0;
    std::string user = "ALICE";
    WorkerCall sent;
    std::vector<McpCallRecord> records;
    SessionWorkerProviderConfig config;
    config.policy = policy;
    config.connection = 7;
    config.session_identity = "/app/con[0]/ses[0]|key|generation";
    config.specs = all_tool_specs();
    config.audit = [&](const McpCallRecord& record) { records.push_back(record); };
    config.probe = [&](int id) { CHECK(id == 7); ++probes; return probe_reply(user); };
    config.invoke = [&](const WorkerCall& call, const std::function<void()>& before_send) {
        before_send();
        ++actions;
        sent = call;
        return json{{"status", "success"}, {"data", {{"title", "Easy Access"}}}};
    };
    auto provider = make_session_worker_provider(std::move(config));
    auto ctx = caller();
    const auto ok = provider->call_tool("gui_screen_read", {{"connection", 7}, {"no_tabs", true}}, ctx);
    const std::string detail = ok.structured ? ok.structured->dump() : (ok.content.empty() ? "empty" : ok.content[0].dump());
    INFO(detail);
    CHECK_FALSE(ok.is_error);
    CHECK(actions == 1);
    CHECK(probes >= 2);
    CHECK(sent.connection == 7);
    CHECK(sent.session_identity == "/app/con[0]/ses[0]|key|generation");
    CHECK(sent.owner_identity == "A4H/001/ALICE");
    CHECK(sent.read_only);
    REQUIRE(records.size() == 1);
    REQUIRE(records[0].sap.has_value());
    CHECK(records[0].sap->user == "ALICE");

    user = "BOB";
    CHECK(provider->call_tool("gui_screen_read", {{"connection", 7}, {"no_tabs", true}}, ctx).is_error);
    CHECK(actions == 1);
    user = "ALICE";
    ctx.reauthorize = [] { return false; };
    CHECK(provider->call_tool("gui_screen_read", {{"connection", 7}, {"no_tabs", true}}, ctx).is_error);
    CHECK(actions == 1);
}

TEST_CASE("routed provider rechecks token at the worker submission boundary", "[mcp][worker-provider]") {
    Policy policy;
    policy.owner_sap_identities = {"A4H/001/ALICE"};
    bool valid = true;
    int submitted = 0;
    SessionWorkerProviderConfig config;
    config.policy = policy;
    config.connection = 7;
    config.session_identity = "/app/con[0]/ses[0]|key|generation";
    config.probe = [](int) { return probe_reply(); };
    config.invoke = [&](const WorkerCall&, const std::function<void()>& before_send) {
        valid = false; // represents revocation while waiting for the private pipe
        before_send();
        ++submitted;
        return json{{"status", "success"}, {"data", json::object()}};
    };
    auto provider = make_session_worker_provider(std::move(config));
    auto ctx = caller();
    ctx.reauthorize = [&] { return valid; };
    CHECK(provider->call_tool("gui_screen_read", {{"connection", 7}, {"no_tabs", true}}, ctx).is_error);
    CHECK(submitted == 0);
}

TEST_CASE("routed disconnect needs a lease and runs in the bound worker", "[mcp][worker-provider]") {
    Policy policy;
    policy.read_only = false;
    policy.allow_write = true;
    policy.owner_sap_identities = {"A4H/001/ALICE"};
    const std::string identity = "/app/con[0]/ses[0]|key|generation";
    auto leases = std::make_shared<SessionLeases>(std::chrono::seconds(60));
    auto routing = std::make_shared<SessionRoutingState>();
    routing->set_sticky_connection("token-1", 7, identity);
    routing->set_sticky_connection("token-2", 7, identity);
    routing->set_sticky_connection("token-3", 8, "/app/con[0]/ses[1]|key|generation");
    int actions = 0;
    int retired = 0;
    WorkerCall sent;
    SessionWorkerProviderConfig config;
    config.policy = policy;
    config.connection = 7;
    config.session_identity = identity;
    config.specs = all_tool_specs();
    config.leases = leases;
    config.routing = routing;
    config.on_disconnect = [&](int id, const std::string& old_identity) {
        CHECK(id == 7);
        CHECK(old_identity == identity);
        ++retired;
    };
    config.probe = [](int) { return probe_reply(); };
    config.invoke = [&](const WorkerCall& call, const std::function<void()>& before_send) {
        before_send();
        ++actions;
        sent = call;
        return json{{"status", "success"}, {"data", {{"connection_id", 7}, {"session_closed", true}}}};
    };
    auto provider = make_session_worker_provider(std::move(config));
    const auto ctx = caller();
    CHECK(provider->call_tool("gui_session_disconnect", {{"connection", 7}, {"close_session", true}}, ctx).is_error);
    CHECK(actions == 0);
    const auto lease = leases->acquire(identity, ctx.principal.id, SessionLeases::Clock::now());
    REQUIRE(lease.status == LeaseStatus::Granted);
    const auto result = provider->call_tool("gui_session_disconnect",
        {{"connection", 7}, {"close_session", true}, {"lease_id", lease.lease_id}}, ctx);
    INFO((result.structured ? result.structured->dump() : result.content.dump()));
    CHECK_FALSE(result.is_error);
    CHECK(actions == 1);
    CHECK(retired == 1);
    CHECK(sent.argv == std::vector<std::string>{"session", "disconnect", "--connection", "7", "--close-session"});
    CHECK_FALSE(leases->permits_write(identity, ctx.principal.id, lease.lease_id, SessionLeases::Clock::now()));
    CHECK_FALSE(routing->sticky_target("token-1").has_value());
    CHECK_FALSE(routing->sticky_target("token-2").has_value());
    CHECK(routing->sticky_target("token-3").has_value());
}

TEST_CASE("read-only disconnect of a saved connection keeps SAP open", "[mcp][worker-provider]") {
    Policy policy;
    policy.read_only = true;
    policy.owner_sap_identities = {"A4H/001/ALICE"};
    const std::string identity = "/app/con[0]/ses[0]|key|generation";
    auto leases = std::make_shared<SessionLeases>(std::chrono::seconds(60));
    int actions = 0;
    SessionWorkerProviderConfig config;
    config.policy = policy;
    config.connection = 7;
    config.session_identity = identity;
    config.specs = all_tool_specs();
    config.leases = leases;
    config.probe = [](int) { return probe_reply(); };
    config.invoke = [&](const WorkerCall& call, const std::function<void()>& gate) {
        gate();
        ++actions;
        CHECK(call.argv == std::vector<std::string>{"session", "disconnect", "--connection", "7"});
        CHECK(call.read_only);
        return json{{"status", "success"}, {"data", {{"connection_id", 7}, {"session_closed", false}}}};
    };
    auto provider = make_session_worker_provider(std::move(config));
    auto ctx = caller();
    const auto lease = leases->acquire(identity, ctx.principal.id, SessionLeases::Clock::now());
    REQUIRE(lease.status == LeaseStatus::Granted);
    const auto result = provider->call_tool("gui_session_disconnect",
        {{"connection", 7}, {"lease_id", lease.lease_id}}, ctx);
    INFO((result.structured ? result.structured->dump() : result.content.dump()));
    CHECK_FALSE(result.is_error);
    CHECK(actions == 1);
}

TEST_CASE("uncertain disconnect outcome retires cached routing", "[mcp][worker-provider]") {
    Policy policy;
    policy.allow_write = true;
    policy.owner_sap_identities = {"A4H/001/ALICE"};
    const std::string identity = "/app/con[0]/ses[0]|key|generation";
    auto leases = std::make_shared<SessionLeases>(std::chrono::seconds(60));
    int retired = 0;
    SessionWorkerProviderConfig config;
    config.policy = policy;
    config.connection = 7;
    config.session_identity = identity;
    config.specs = all_tool_specs();
    config.leases = leases;
    config.on_disconnect = [&](int, const std::string&) { ++retired; };
    config.probe = [](int) { return probe_reply(); };
    config.invoke = [](const WorkerCall&, const std::function<void()>& gate) {
        gate();
        return json{{"status", "error"}, {"error", {{"code", "OUTCOME_UNKNOWN"}, {"message", "worker exited"}}}};
    };
    auto provider = make_session_worker_provider(std::move(config));
    auto ctx = caller();
    const auto lease = leases->acquire(identity, ctx.principal.id, SessionLeases::Clock::now());
    REQUIRE(lease.status == LeaseStatus::Granted);
    const auto result = provider->call_tool("gui_session_disconnect",
        {{"connection", 7}, {"lease_id", lease.lease_id}}, ctx);
    CHECK(result.is_error);
    CHECK(retired == 1);
}
