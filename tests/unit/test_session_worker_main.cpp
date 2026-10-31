// The production session worker's admission of a call (review 2026-10-25 test gap 5): bound to session A it refuses
// B, a changed owner, a stale screen guard and a session whose live facts are not the call's.
#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <stdexcept>

#include "include/cli_handler.h"
#include "include/connection_manager.h"
#include "include/credential_store.h"
#include "include/mcp/screen_guard.h"
#include "include/mcp/session_worker_main.h"
#include "support/fake_engine.h"
#include "support/temp_dir.h"

using namespace fairyfly;
using namespace fairyfly::mcp;

namespace {

audit::SapFacts live_session(int connection, const std::string& identity, const std::string& user) {
    audit::SapFacts f;
    f.connection_id = connection;
    f.session_identity = identity;
    f.system = "S4H";
    f.client = "100";
    f.user = user;
    f.transaction = "SU01";
    f.program = "SAPLSUU5";
    f.screen_number = "0050";
    return f;
}

WorkerCall call_for(int connection, const std::string& identity, const std::string& owner) {
    WorkerCall c;
    c.id = 1;
    c.connection = connection;
    c.session_identity = identity;
    c.owner_identity = owner;
    c.argv = {"screen", "read"};
    return c;
}

std::string code(const Result& r) { return r.error.is_object() ? r.error.value("code", std::string()) : std::string(); }
std::string code(const std::optional<Result>& refusal) { return refusal ? code(*refusal) : std::string(); }

}  // namespace

TEST_CASE("session worker: the first matching call binds it; a mismatch later is refused before SAP is asked", "[mcp][worker-main]") {
    // codex 2026-10-25: through the production invoker, not a re-creation of its order
    fairyfly::test::TempDir dir("worker-binding");
    fairyfly::test::FakeScripting scripting;
    scripting.facts = live_session(0, "", "ALICE");   // the handler adds the connection id and the session identity
    const auto saved = cli::ConnectionManager(dir.path().string())
                           .create_or_update_connection("/app/con[0]/ses[0]", "/app/con[0]", "S4H", "route", "SAP", "key-1");
    const std::string identity = saved.session_id + "|" + saved.server_session_key + "|" + saved.cache_generation;
    int handlers = 0;
    const WorkerInvoker invoke = make_session_worker_invoker([&] {
        ++handlers;
        auto engine = std::make_unique<fairyfly::test::FakeEngine>();
        engine->scripting_fake = &scripting;
        engine->live_sessions["/app/con[0]/ses[0]"] = true;
        return std::make_unique<cli::CommandHandler>(std::make_unique<cred::InMemoryCredentialStore>(), std::move(engine),
                                                     std::make_unique<cli::ConnectionManager>(dir.path().string()));
    });
    const auto first = invoke(call_for(saved.id, identity, "S4H/100/ALICE"));
    INFO(first.to_json().dump());
    CHECK(first.status == Result::Status::Success);   // admitted and run: the worker is bound now
    CHECK(handlers == 1);
    const int reads = scripting.facts_reads;
    CHECK(code(invoke(call_for(saved.id, identity + "-other", "S4H/100/ALICE"))) == "WORKER_SESSION_MISMATCH");
    CHECK(code(invoke(call_for(saved.id, identity, "S4H/100/BOB"))) == "WORKER_SESSION_MISMATCH");
    CHECK(code(invoke(call_for(saved.id + 1, identity, "S4H/100/ALICE"))) == "WORKER_SESSION_MISMATCH");
    CHECK(scripting.facts_reads == reads);   // refused before SAP was asked for the live session
    CHECK(handlers == 1);
}

TEST_CASE("Worker discovery distinguishes a stopped SAP process from failed enumeration", "[mcp][worker-main][startup][startup-process-owner]") {
    fairyfly::test::TempDir dir("worker-startup-discovery");
    bool stopped = true;
    SECTION("confirmed stopped process is an empty listing") {}
    SECTION("enumeration failure is not an empty listing") { stopped = false; }
    const WorkerInvoker invoke = make_session_worker_invoker([&] {
        auto engine = std::make_unique<fairyfly::test::FakeEngine>();
        if (stopped) engine->status = {false, false, "not running"};
        else engine->application_info = nullptr;
        return std::make_unique<cli::CommandHandler>(std::make_unique<cred::InMemoryCredentialStore>(), std::move(engine),
            std::make_unique<cli::ConnectionManager>(dir.path().string()));
    });
    WorkerCall call;
    call.enumerate = true;
    const auto result = invoke(call);
    if (stopped) {
        REQUIRE(result.status == Result::Status::Success);
        CHECK(result.data["connections"] == json::array());
        CHECK(result.data["total_sessions"] == 0);
    } else CHECK(code(result) == "OWNER_IDENTITY_UNKNOWN");
}

TEST_CASE("session worker: the live session must be the call's, with its owner", "[mcp][worker-main]") {
    const std::optional<WorkerBinding> unbound;
    const auto call = call_for(3, "ses-A", "S4H/100/ALICE");
    CHECK(code(worker_live_refusal(call, live_session(3, "ses-A", "BOB"))) == "OWNER_SESSION_UNAVAILABLE");
    CHECK(code(worker_live_refusal(call, live_session(3, "ses-X", "ALICE"))) == "OWNER_SESSION_UNAVAILABLE");
    CHECK(code(worker_live_refusal(call, live_session(5, "ses-A", "ALICE"))) == "OWNER_SESSION_UNAVAILABLE");
    CHECK(code(worker_live_refusal(call, live_session(3, "ses-A", ""))) == "OWNER_SESSION_UNAVAILABLE");
}

TEST_CASE("session worker: a stale screen guard and selection input under FAIRYFLY_READ_ONLY are refused", "[mcp][worker-main]") {
    const std::optional<WorkerBinding> unbound;
    const auto live = live_session(3, "ses-A", "ALICE");
    auto call = call_for(3, "ses-A", "S4H/100/ALICE");
    call.expected_screen_guard = screen_guard_for(live);
    CHECK_FALSE(worker_live_refusal(call, live));
    auto moved = live;
    moved.screen_number = "0100";   // the dynpro changed since the client's screen read
    CHECK(code(worker_live_refusal(call, moved)) == "SCREEN_CHANGED");
    call.selection_input = true;
    CHECK(code(worker_binding_refusal(call, unbound, true)) == "READ_ONLY");
}

TEST_CASE("session worker: a call's handler settings are put back, also when the call throws", "[mcp][worker-main]") {
    // review 2026-10-25: read-only, selection input and the expected session must not leak into the next call
    fairyfly::test::TempDir dir("worker-flags");
    cli::CommandHandler handler(std::make_unique<cred::InMemoryCredentialStore>(), std::make_unique<fairyfly::test::FakeEngine>(),
                                std::make_unique<cli::ConnectionManager>(dir.path().string()));
    WorkerCall call = call_for(3, "ses-A", "S4H/100/ALICE");
    call.read_only = true;           // a read-only token ...
    call.selection_input = true;     // ... with an authorized selection-input fill
    call.selection_program = "SAPLSUU5";
    call.selection_screen = "0050";
    try {
        const WorkerCallFlags flags(handler, call, false);
        CHECK(handler.selection_input_only());
        CHECK_FALSE(handler.read_only());   // the fill lifts the guard, for this call only
        CHECK(handler.expected_mcp_session() == "ses-A");
        CHECK(handler.expected_mcp_owner() == "S4H/100/ALICE");
        throw std::runtime_error("the command failed half way");
    } catch (const std::runtime_error&) {
    }
    CHECK(handler.read_only());
    CHECK_FALSE(handler.selection_input_only());
    CHECK(handler.expected_mcp_session().empty());   // the next call does not inherit the expected session
    CHECK(handler.expected_mcp_owner().empty());
    // FAIRYFLY_READ_ONLY keeps the guard on whatever the call asks
    call.selection_input = false;
    {
        const WorkerCallFlags capped(handler, call, true);
        CHECK(handler.read_only());
    }
}
