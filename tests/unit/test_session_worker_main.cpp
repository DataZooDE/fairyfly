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

std::string code(const std::optional<Result>& refusal) { return refusal ? refusal->error.value("code", "") : ""; }

/// The worker's whole admission: the binding first (before SAP), then the live session.
std::optional<Result> worker_call_refusal(const WorkerCall& call, const std::optional<WorkerBinding>& bound,
                                          const audit::SapFacts& live, bool read_only_cap) {
    if (auto refused = worker_binding_refusal(call, bound, read_only_cap)) return refused;
    return worker_live_refusal(call, live);
}

}  // namespace

TEST_CASE("session worker: the first matching call binds it; later calls must be for the same session and owner", "[mcp][worker-main]") {
    std::optional<WorkerBinding> bound;
    const auto a = live_session(3, "ses-A", "ALICE");
    CHECK_FALSE(worker_call_refusal(call_for(3, "ses-A", "S4H/100/ALICE"), bound, a, false));
    bound = WorkerBinding{3, "ses-A", "S4H/100/ALICE"};
    CHECK_FALSE(worker_call_refusal(call_for(3, "ses-A", "S4H/100/ALICE"), bound, a, false));
    // another session, another saved connection, another owner: refused before SAP is touched
    CHECK(code(worker_call_refusal(call_for(3, "ses-B", "S4H/100/ALICE"), bound, a, false)) == "WORKER_SESSION_MISMATCH");
    CHECK(code(worker_call_refusal(call_for(4, "ses-A", "S4H/100/ALICE"), bound, a, false)) == "WORKER_SESSION_MISMATCH");
    CHECK(code(worker_call_refusal(call_for(3, "ses-A", "S4H/100/BOB"), bound, a, false)) == "WORKER_SESSION_MISMATCH");
}

TEST_CASE("session worker: the live session must be the call's, with its owner", "[mcp][worker-main]") {
    const std::optional<WorkerBinding> unbound;
    const auto call = call_for(3, "ses-A", "S4H/100/ALICE");
    CHECK(code(worker_call_refusal(call, unbound, live_session(3, "ses-A", "BOB"), false)) == "OWNER_SESSION_UNAVAILABLE");
    CHECK(code(worker_call_refusal(call, unbound, live_session(3, "ses-X", "ALICE"), false)) == "OWNER_SESSION_UNAVAILABLE");
    CHECK(code(worker_call_refusal(call, unbound, live_session(5, "ses-A", "ALICE"), false)) == "OWNER_SESSION_UNAVAILABLE");
    CHECK(code(worker_call_refusal(call, unbound, live_session(3, "ses-A", ""), false)) == "OWNER_SESSION_UNAVAILABLE");
}

TEST_CASE("session worker: a stale screen guard and selection input under FAIRYFLY_READ_ONLY are refused", "[mcp][worker-main]") {
    const std::optional<WorkerBinding> unbound;
    const auto live = live_session(3, "ses-A", "ALICE");
    auto call = call_for(3, "ses-A", "S4H/100/ALICE");
    call.expected_screen_guard = screen_guard_for(live);
    CHECK_FALSE(worker_call_refusal(call, unbound, live, false));
    auto moved = live;
    moved.screen_number = "0100";   // the dynpro changed since the client's screen read
    CHECK(code(worker_call_refusal(call, unbound, moved, false)) == "SCREEN_CHANGED");
    call.selection_input = true;
    CHECK(code(worker_call_refusal(call, unbound, live, true)) == "READ_ONLY");
}

TEST_CASE("session worker: a call's handler settings are put back, also when the call throws", "[mcp][worker-main]") {
    // review 2026-10-25: read-only, selection input and the expected session must not leak into the next call
    fairyfly::test::TempDir dir("worker-flags");
    cli::CommandHandler handler(std::make_unique<cred::InMemoryCredentialStore>(), std::make_unique<fairyfly::test::FakeEngine>(),
                                std::make_unique<cli::ConnectionManager>(dir.path().string()));
    WorkerCall call = call_for(3, "ses-A", "S4H/100/ALICE");
    call.read_only = false;
    call.selection_input = true;
    call.selection_program = "SAPLSUU5";
    call.selection_screen = "0050";
    try {
        const WorkerCallFlags flags(handler, call, false);
        CHECK(handler.selection_input_only());
        CHECK_FALSE(handler.read_only());   // selection input lifts the guard for this call only
        throw std::runtime_error("the command failed half way");
    } catch (const std::runtime_error&) {
    }
    CHECK(handler.read_only());
    CHECK_FALSE(handler.selection_input_only());
    // FAIRYFLY_READ_ONLY keeps the guard on whatever the call asks
    call.selection_input = false;
    {
        const WorkerCallFlags capped(handler, call, true);
        CHECK(handler.read_only());
    }
}
