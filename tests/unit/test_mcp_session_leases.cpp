#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <atomic>
#include <future>
#include <thread>

#include "include/mcp/session_leases.h"
#include "include/mcp/session_policy_state.h"
#include "include/mcp/session_token_status.h"
#include "include/mcp/session_state_cleanup.h"

using namespace fairyfly::mcp;

TEST_CASE("missing token metadata does not prove revocation", "[mcp][lease][security]") {
    const auto now = fairyfly::auth::TimePoint{};
    CHECK_FALSE(token_definitively_invalid(std::nullopt, now));
    fairyfly::auth::TokenMeta meta;
    CHECK_FALSE(token_definitively_invalid(meta, now));
    meta.revoked = true;
    CHECK(token_definitively_invalid(meta, now));
    meta.revoked = false;
    meta.expires = now;
    CHECK(token_definitively_invalid(meta, now));
}

TEST_CASE("session leases: one token owns a live session at a time", "[mcp][lease]") {
    const auto now = SessionLeases::Clock::time_point{};
    int next = 0;
    SessionLeases leases(std::chrono::seconds(60), [&] { return "lease-" + std::to_string(++next); });

    const auto first = leases.acquire("session-A|key|generation", "token-A", now);
    REQUIRE(first.status == LeaseStatus::Granted);
    CHECK(first.lease_id == "lease-1");
    CHECK(leases.permits_write("session-A|key|generation", "token-A", first.lease_id, now));
    CHECK_FALSE(leases.permits_write("session-A|key|generation", "token-B", first.lease_id, now));
    CHECK(leases.acquire("session-A|key|generation", "token-B", now).status == LeaseStatus::HeldByOther);
    // Another client can share a bearer token. Reacquiring must not disclose the current lease secret.
    const auto duplicate = leases.acquire("session-A|key|generation", "token-A", now);
    CHECK(duplicate.status == LeaseStatus::HeldByOther);
    CHECK(duplicate.lease_id.empty());
    CHECK(leases.acquire("session-B|key|generation", "token-B", now).status == LeaseStatus::Granted);

    CHECK_FALSE(leases.release("session-A|key|generation", "token-B", first.lease_id));
    CHECK_FALSE(leases.release("session-A|key|generation", "token-A", "wrong"));
    CHECK(leases.release("session-A|key|generation", "token-A", first.lease_id));
    CHECK(leases.acquire("session-A|key|generation", "token-B", now).status == LeaseStatus::Granted);
}

TEST_CASE("session leases: expiry and generation changes cannot retain write access", "[mcp][lease]") {
    const auto now = SessionLeases::Clock::time_point{};
    int next = 0;
    SessionLeases leases(std::chrono::seconds(60), [&] { return "lease-" + std::to_string(++next); });
    const auto first = leases.acquire("session-A|key|generation-1", "token-A", now);
    REQUIRE(first.status == LeaseStatus::Granted);
    CHECK_FALSE(leases.permits_write("session-A|key|generation-2", "token-A", first.lease_id, now));
    CHECK_FALSE(leases.permits_write("session-A|key|generation-1", "token-A", first.lease_id,
                                     now + std::chrono::seconds(60)));
    CHECK(leases.acquire("session-A|key|generation-1", "token-B", now + std::chrono::seconds(60)).status ==
          LeaseStatus::Granted);
    CHECK(leases.renew("session-A|key|generation-1", "token-A", first.lease_id,
                       now + std::chrono::seconds(61)).status != LeaseStatus::Granted);
}

TEST_CASE("session leases: reattach cannot grant a second lease during an old write", "[mcp][lease][security]") {
    const auto now = SessionLeases::Clock::time_point{};
    int next = 0;
    SessionLeases leases(std::chrono::seconds(60), [&] { return "lease-" + std::to_string(++next); });
    const std::string before = "/app/con[0]/ses[0]|server-key|generation-1";
    const std::string after = "/app/con[0]/ses[0]|server-key|generation-2";
    const auto first = leases.acquire(before, "token-A", now);
    REQUIRE(first.status == LeaseStatus::Granted);
    REQUIRE(leases.begin_write(before, "token-A", first.lease_id, now));
    CHECK_FALSE(leases.permits_write(after, "token-A", first.lease_id, now));
    leases.retire_session(after); // a reattach/close invalidates the old generation
    CHECK(leases.acquire(after, "token-B", now).status == LeaseStatus::HeldByOther);
    leases.end_write(before, "token-A", first.lease_id, now);
    CHECK(leases.acquire(after, "token-B", now).status == LeaseStatus::Granted);
}

TEST_CASE("session leases: revocation and closure retire ownership", "[mcp][lease]") {
    const auto now = SessionLeases::Clock::time_point{};
    int next = 0;
    SessionLeases leases(std::chrono::seconds(60), [&] { return "lease-" + std::to_string(++next); });
    const auto a = leases.acquire("session-A", "token-A", now);
    const auto b = leases.acquire("session-B", "token-A", now);
    REQUIRE(a.status == LeaseStatus::Granted);
    REQUIRE(b.status == LeaseStatus::Granted);
    leases.retire_session("session-A");
    CHECK_FALSE(leases.permits_write("session-A", "token-A", a.lease_id, now));
    CHECK(leases.permits_write("session-B", "token-A", b.lease_id, now));
    leases.revoke_token("token-A");
    CHECK_FALSE(leases.permits_write("session-B", "token-A", b.lease_id, now));
}

TEST_CASE("session leases: an active write pins an expired lease until completion", "[mcp][lease]") {
    const auto now = SessionLeases::Clock::time_point{};
    SessionLeases leases(std::chrono::seconds(60), [] { return "lease-A"; });
    const auto a = leases.acquire("session-A", "token-A", now);
    REQUIRE(a.status == LeaseStatus::Granted);
    REQUIRE(leases.begin_write("session-A", "token-A", a.lease_id, now));
    CHECK_FALSE(leases.release("session-A", "token-A", a.lease_id));
    CHECK(leases.acquire("session-A", "token-B", now + std::chrono::seconds(61)).status == LeaseStatus::HeldByOther);
    CHECK(leases.renew("session-A", "token-A", a.lease_id, now + std::chrono::seconds(61)).status == LeaseStatus::NotHeld);
    leases.end_write("session-A", "token-A", a.lease_id, now + std::chrono::seconds(61));
    CHECK(leases.acquire("session-A", "token-B", now + std::chrono::seconds(61)).status == LeaseStatus::Granted);
}

TEST_CASE("session leases: revoking a token cannot transfer an in-flight write", "[mcp][lease]") {
    const auto now = SessionLeases::Clock::time_point{};
    SessionLeases leases(std::chrono::seconds(60), [] { return "lease-A"; });
    const auto a = leases.acquire("session-A", "token-A", now);
    REQUIRE(a.status == LeaseStatus::Granted);
    REQUIRE(leases.begin_write("session-A", "token-A", a.lease_id, now));
    leases.revoke_token("token-A");
    CHECK_FALSE(leases.begin_write("session-A", "token-A", a.lease_id, now));
    CHECK(leases.acquire("session-A", "token-B", now).status == LeaseStatus::HeldByOther);
    leases.end_write("session-A", "token-A", a.lease_id, now);
    CHECK(leases.acquire("session-A", "token-B", now).status == LeaseStatus::Granted);
}

TEST_CASE("session leases: a revoked holder releases its idle lease on the next acquire", "[mcp][lease]") {
    const auto now = SessionLeases::Clock::time_point{};
    bool token_a_valid = true;
    int next = 0;
    SessionLeases leases(std::chrono::seconds(60), [&] { return "lease-" + std::to_string(++next); },
                         [&](const std::string& id) { return id != "token-A" || token_a_valid; });
    const auto a = leases.acquire("session-A", "token-A", now);
    REQUIRE(a.status == LeaseStatus::Granted);
    CHECK(leases.acquire("session-A", "token-B", now).status == LeaseStatus::HeldByOther);
    token_a_valid = false;
    const auto b = leases.acquire("session-A", "token-B", now);
    CHECK(b.status == LeaseStatus::Granted);
    CHECK_FALSE(leases.permits_write("session-A", "token-A", a.lease_id, now));
    CHECK(leases.permits_write("session-A", "token-B", b.lease_id, now));
}

TEST_CASE("session leases: a revoked holder keeps an in-flight write pinned", "[mcp][lease]") {
    const auto now = SessionLeases::Clock::time_point{};
    bool token_a_valid = true;
    int next = 0;
    SessionLeases leases(std::chrono::seconds(60), [&] { return "lease-" + std::to_string(++next); },
                         [&](const std::string& id) { return id != "token-A" || token_a_valid; });
    const auto a = leases.acquire("session-A", "token-A", now);
    REQUIRE(a.status == LeaseStatus::Granted);
    REQUIRE(leases.begin_write("session-A", "token-A", a.lease_id, now));
    token_a_valid = false;
    CHECK(leases.acquire("session-A", "token-B", now).status == LeaseStatus::HeldByOther);
    CHECK_FALSE(leases.permits_write("session-A", "token-A", a.lease_id, now));
    leases.end_write("session-A", "token-A", a.lease_id, now);
    CHECK(leases.acquire("session-A", "token-B", now).status == LeaseStatus::Granted);
}

TEST_CASE("session policy state retires closed session and revoked token records", "[mcp][policy_state]") {
    SessionPolicyState state;
    const std::string a = "token-A\x1fsession:old";
    const std::string b = "token-B\x1fsession:old";
    const std::string other = "token-A\x1fsession:other";
    const std::string fallback = "token-A\x1f" "connection:7";
    state.set_tcode_blocked(a, true);
    state.set_tcode_blocked(b, true);
    state.set_tcode_blocked(other, true);
    state.set_tcode_blocked(fallback, true);
    state.set_initial_screen(a, fairyfly::auth::InitialScreen{});
    state.retire_session("old", 7);
    CHECK_FALSE(state.tcode_blocked(a));
    CHECK_FALSE(state.tcode_blocked(b));
    CHECK_FALSE(state.tcode_blocked(fallback));
    CHECK_FALSE(state.initial_screen(a));
    CHECK(state.tcode_blocked(other));
    state.revoke_token("token-A");
    CHECK_FALSE(state.tcode_blocked(other));
}

TEST_CASE("session policy state keeps a token block across saved-connection detach", "[mcp][policy_state]") {
    CHECK(policy_session_identity("/app/con[0]/ses[0]|server-key|generation-1") ==
          "/app/con[0]/ses[0]|server-key");
    CHECK(policy_session_identity("/app/con[0]/ses[0]|server-key|generation-2") ==
          "/app/con[0]/ses[0]|server-key");
    SessionPolicyState state;
    const std::string session = "token-A\x1f" "session:/app/con[0]/ses[0]|server-key";
    const std::string fallback = "token-A\x1f" "connection:7";
    state.set_tcode_blocked(session, true);
    state.set_tcode_blocked(fallback, true);
    state.retire_connection(7); // detach leaves SAP window alive
    CHECK(state.tcode_blocked(session));
    CHECK_FALSE(state.tcode_blocked(fallback));
    state.retire_session("/app/con[0]/ses[0]|server-key|generation-2", 8); // actual close
    CHECK_FALSE(state.tcode_blocked(session));
}

TEST_CASE("prelogin claims bind one saved generation to one token and expire", "[mcp][routing][prelogin]") {
    SessionRoutingState routing;
    const auto now = std::chrono::steady_clock::time_point{};
    REQUIRE(routing.bind_prelogin("token-a", 7, "window|key|generation-1", now));
    CHECK(routing.owns_prelogin("token-a", 7, "window|key|generation-1", now));
    CHECK_FALSE(routing.owns_prelogin("token-b", 7, "window|key|generation-1", now));
    CHECK_FALSE(routing.owns_prelogin("token-a", 7, "window|key|generation-2", now));
    CHECK_FALSE(routing.bind_prelogin("token-b", 7, "window|key|generation-1", now));
    CHECK(routing.bind_prelogin("token-b", 7, "window|key|generation-2", now));
    CHECK_FALSE(routing.owns_prelogin("token-a", 7, "window|key|generation-1", now));
    CHECK(routing.owns_prelogin("token-b", 7, "window|key|generation-2", now));
    CHECK_FALSE(routing.owns_prelogin("token-b", 7, "window|key|generation-2", now + std::chrono::minutes(16)));
    REQUIRE(routing.bind_prelogin("token-a", 8, "other|key|generation", now));
    routing.revoke_principal("token-a");
    CHECK_FALSE(routing.owns_prelogin("token-a", 8, "other|key|generation", now));
}

TEST_CASE("one token cannot consume every prelogin claim slot", "[mcp][routing][prelogin]") {
    SessionRoutingState routing;
    for (int i = 0; i < 8; ++i)
        REQUIRE(routing.bind_prelogin("token-a", i, "window-" + std::to_string(i) + "||generation"));
    CHECK_FALSE(routing.prelogin_capacity_available("token-a"));
    CHECK(routing.prelogin_capacity_available("token-b"));
    CHECK_FALSE(routing.bind_prelogin("token-a", 8, "new-window||generation"));
    CHECK(routing.bind_prelogin("token-b", 8, "new-window||generation"));
}

TEST_CASE("session state cleanup removes only definitively invalid token state", "[mcp][policy_state][security]") {
    SessionPolicyState policy;
    SessionRoutingState routing;
    SessionLeases leases(std::chrono::seconds(60), [] { return "lease"; });
    const auto now = SessionLeases::Clock::time_point{};
    for (const auto& id : {"removed", "unreadable", "active", "revoked"}) {
        policy.set_tcode_blocked(std::string(id) + "\x1f" "session:window", true);
        routing.set_sticky_connection(id, 2, "window|key|generation");
    }
    const auto old = leases.acquire("window|key|generation", "removed", now);
    REQUIRE(old.status == LeaseStatus::Granted);
    const auto lease_only = leases.acquire("other-window|key|generation", "lease-only", now);
    REQUIRE(lease_only.status == LeaseStatus::Granted);
    routing.set_sticky_connection("sticky-only", 3, "other-window|key|generation");
    REQUIRE(routing.bind_prelogin("claim-only", 9, "prelogin|key|generation"));
    const auto cleanup_now = fairyfly::auth::TimePoint{};
    int scans = 0;
    cleanup_invalid_token_state(policy, leases, routing,
        [&] {
            ++scans;
            fairyfly::auth::FreshTokenSnapshot snapshot;
            snapshot.complete = true;
            for (const auto& id : {"unreadable", "active", "revoked"}) {
                fairyfly::auth::TokenMeta meta;
                meta.id = id;
                meta.revoked = id == std::string("revoked");
                snapshot.by_id[id] = meta;
            }
            return snapshot;
        }, cleanup_now);
    CHECK(scans == 1);
    CHECK_FALSE(policy.tcode_blocked("removed\x1f" "session:window"));
    CHECK_FALSE(policy.tcode_blocked("revoked\x1f" "session:window"));
    CHECK(policy.tcode_blocked("unreadable\x1f" "session:window"));
    CHECK(policy.tcode_blocked("active\x1f" "session:window"));
    CHECK_FALSE(routing.sticky_target("removed"));
    CHECK(routing.sticky_target("unreadable"));
    CHECK_FALSE(leases.permits_write("window|key|generation", "removed", old.lease_id, now));
    CHECK_FALSE(leases.permits_write("other-window|key|generation", "lease-only", lease_only.lease_id, now));
    CHECK_FALSE(routing.sticky_target("sticky-only"));
    CHECK_FALSE(routing.owns_prelogin("claim-only", 9, "prelogin|key|generation"));
}

TEST_CASE("session state cleanup runs periodically and stops with its owner", "[mcp][policy_state]") {
    std::atomic<int> runs{0};
    {
        PeriodicSessionStateCleanup cleanup(std::chrono::milliseconds(10), [&] { ++runs; });
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (runs == 0 && std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        REQUIRE(runs > 0);
    }
    const int after_stop = runs;
    std::this_thread::sleep_for(std::chrono::milliseconds(25));
    CHECK(runs == after_stop);
}

TEST_CASE("incomplete credential scan retains absent token restrictions", "[mcp][policy_state][security]") {
    SessionPolicyState policy;
    SessionRoutingState routing;
    SessionLeases leases(std::chrono::seconds(60), [] { return "lease"; });
    policy.set_tcode_blocked("uncertain\x1f" "session:window", true);
    const auto held = leases.acquire("window|key|generation", "uncertain", SessionLeases::Clock::time_point{});
    REQUIRE(held.status == LeaseStatus::Granted);
    cleanup_invalid_token_state(policy, leases, routing, [] {
        fairyfly::auth::FreshTokenSnapshot snapshot;
        snapshot.complete = false;
        return snapshot;
    }, fairyfly::auth::TimePoint{});
    CHECK(policy.tcode_blocked("uncertain\x1f" "session:window"));
    CHECK(leases.permits_write("window|key|generation", "uncertain", held.lease_id,
                               SessionLeases::Clock::time_point{}));
}

TEST_CASE("session leases: slow token validation does not block another window", "[mcp][lease]") {
    const auto now = SessionLeases::Clock::time_point{};
    std::atomic<bool> checking{false};
    std::atomic<bool> proceed{false};
    SessionLeases leases(std::chrono::seconds(60), [] { return "lease"; }, [&](const std::string&) {
        checking = true;
        while (!proceed) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        return true;
    });
    const auto a = leases.acquire("session-A", "token-A", now);
    const auto b = leases.acquire("session-B", "token-B", now);
    REQUIRE(a.status == LeaseStatus::Granted);
    REQUIRE(b.status == LeaseStatus::Granted);
    auto contested = std::async(std::launch::async, [&] { return leases.acquire("session-A", "token-C", now); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!checking && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    if (!checking.load()) {
        proceed = true;
        contested.wait();
        FAIL("token validation did not start");
    }
    auto other = std::async(std::launch::async, [&] {
        return leases.permits_write("session-B", "token-B", b.lease_id, now);
    });
    const bool responsive = other.wait_for(std::chrono::milliseconds(200)) == std::future_status::ready;
    proceed = true;
    CHECK(responsive);
    CHECK(other.get());
    CHECK(contested.get().status == LeaseStatus::HeldByOther);
}

TEST_CASE("session leases: expiry advances while token validation waits", "[mcp][lease]") {
    const auto now = SessionLeases::Clock::time_point{};
    int next = 0;
    SessionLeases leases(std::chrono::milliseconds(10), [&] { return "lease-" + std::to_string(++next); },
                         [](const std::string&) {
                             std::this_thread::sleep_for(std::chrono::milliseconds(40));
                             return true;
                         });
    REQUIRE(leases.acquire("session-A", "token-A", now).status == LeaseStatus::Granted);
    const auto next_lease = leases.acquire("session-A", "token-B", now);
    REQUIRE(next_lease.status == LeaseStatus::Granted);
    CHECK(next_lease.expires_at >= now + std::chrono::milliseconds(40));
}

TEST_CASE("session leases: periodic expiry sweep retains active writes only", "[mcp][lease]") {
    const auto now = SessionLeases::Clock::time_point{};
    int next = 0;
    SessionLeases leases(std::chrono::seconds(1), [&] { return "lease-" + std::to_string(++next); });
    const auto idle = leases.acquire("idle|key|generation", "idle-token", now);
    const auto active = leases.acquire("active|key|generation", "active-token", now);
    REQUIRE(idle.status == LeaseStatus::Granted);
    REQUIRE(active.status == LeaseStatus::Granted);
    REQUIRE(leases.begin_write("active|key|generation", "active-token", active.lease_id, now));
    leases.sweep_expired(now + std::chrono::seconds(2));
    CHECK_FALSE(leases.token_ids().count("idle-token"));
    CHECK(leases.token_ids().count("active-token"));
    leases.end_write("active|key|generation", "active-token", active.lease_id, now + std::chrono::seconds(2));
    CHECK_FALSE(leases.token_ids().count("active-token"));
}
