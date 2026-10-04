#include <catch2/catch_test_macros.hpp>

#include <chrono>
#include <filesystem>
#include <thread>

#include "include/mcp/session_worker_broker.h"

#ifdef _WIN32
#include <windows.h>

using namespace fairyfly::mcp;

namespace {
std::wstring fixture_path() {
    wchar_t path[MAX_PATH]{};
    REQUIRE(GetModuleFileNameW(nullptr, path, MAX_PATH) > 0);
    return (std::filesystem::path(path).parent_path() / L"mcp_worker_fixture.exe").wstring();
}
WorkerCall call(int id, std::string identity) {
    WorkerCall c;
    c.id = id;
    c.connection = id;
    c.session_identity = "/app/con[0]/ses[0]|server-key|" + std::move(identity);
    c.owner_identity = "A4H/001/OWNER";
    c.argv = {"screen", "read", "--connection", std::to_string(id)};
    return c;
}
WorkerGateResult allow(const WorkerCall&) { return {}; }
}

TEST_CASE("Broker rejects before allocating a session lane", "[mcp][worker-broker]") {
    SessionWorkerBroker broker(fixture_path(), L"", 1, 2, 5000);
    BrokerSubmitResult status = BrokerSubmitResult::Queued;
    auto denied = broker.submit(call(1, "one"), [](const WorkerCall&) {
        return WorkerGateResult{false, "OWNER_SESSION_UNAVAILABLE", "unavailable"};
    }, allow, &status);
    CHECK(status == BrokerSubmitResult::Rejected);
    CHECK(denied.get()["error"]["code"] == "OWNER_SESSION_UNAVAILABLE");
    auto bad = call(1, "bad");
    bad.argv = {"session", "launch", "--connection", "1"};
    status = BrokerSubmitResult::Queued;
    auto malformed = broker.submit(bad, allow, allow, &status);
    CHECK(status == BrokerSubmitResult::Rejected);
    CHECK(malformed.get()["error"]["code"] == "WORKER_BAD_REQUEST");
    auto accepted = broker.submit(call(2, "two"), allow, allow);
    CHECK(accepted.get()["data"]["id"] == 2);
}

TEST_CASE("Broker probes then binds the same private process to its live session", "[mcp][worker-broker]") {
    SessionWorkerBroker broker(fixture_path(), L"", 1, 2, 5000);
    CHECK(broker.probe(7)["data"]["connection_id"] == 7);
    auto request = call(7, "one");
    CHECK(broker.submit(request, allow, allow).get()["data"]["id"] == 7);
    CHECK(broker.probe(7)["data"]["connection_id"] == 7);
    CHECK(broker.probe(8)["data"]["connection_id"] == 8);
}

TEST_CASE("Owner discovery uses a disposable worker and times out a hung SAP enumeration", "[mcp][worker-broker]") {
    SessionWorkerBroker healthy(fixture_path(), L"", 1, 2, 5000);
    CHECK(healthy.enumerate_sessions(std::chrono::milliseconds(500))["data"]["enumerate"] == true);
    CHECK(healthy.invoke_direct(call(7, "after-list"))["data"]["id"] == 7);

    SessionWorkerBroker stuck(fixture_path(), L"--hang-on-call", 1, 2, 5000);
    const auto started = std::chrono::steady_clock::now();
    const auto denied = stuck.enumerate_sessions(std::chrono::milliseconds(100));
    CHECK(denied["error"]["code"] == "OWNER_IDENTITY_UNKNOWN");
    CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(2));

    const auto cancelled = healthy.enumerate_sessions(std::chrono::seconds(1), [] { return true; });
    CHECK(cancelled["error"]["code"] == "OWNER_IDENTITY_UNKNOWN");
    CHECK(healthy.enumerate_sessions(std::chrono::milliseconds(500))["data"]["enumerate"] == true);
}

TEST_CASE("Denied connection probes cannot exhaust private worker capacity", "[mcp][worker-broker]") {
    SessionWorkerBroker broker(fixture_path(), L"", 1, 2, 5000);
    for (int id = 1; id <= 4; ++id)
        CHECK(broker.probe(id)["data"]["connection_id"] == id);
    CHECK(broker.invoke_direct(call(5, "owner"))["data"]["id"] == 5);
}

TEST_CASE("Broker direct invoke keeps a validated session binding", "[mcp][worker-broker]") {
    SessionWorkerBroker broker(fixture_path(), L"", 1, 2, 5000);
    auto first = call(7, "one");
    CHECK(broker.probe(7)["data"]["connection_id"] == 7);
    CHECK(broker.invoke_direct(first)["data"]["id"] == 7);
    auto replaced = call(7, "replacement");
    CHECK(broker.invoke_direct(replaced)["data"]["id"] == 7);
    bool checked = false;
    try {
        (void)broker.invoke_direct(first, [&] {
            checked = true;
            throw WorkerTransportError("TOKEN_CHANGED", "revoked");
        });
        FAIL("revoked action must not run");
    } catch (const WorkerTransportError& error) {
        CHECK(error.code() == "TOKEN_CHANGED");
    }
    CHECK(checked);
}

TEST_CASE("Broker reuses capacity after an idle bound session", "[mcp][worker-broker]") {
    SessionWorkerBroker broker(fixture_path(), L"", 1, 2, 5000);
    CHECK(broker.invoke_direct(call(7, "first"))["data"]["id"] == 7);
    CHECK(broker.invoke_direct(call(8, "second"))["data"]["id"] == 8);
}

TEST_CASE("Broker retires a disconnected worker without touching a replacement", "[mcp][worker-broker]") {
    SessionWorkerBroker broker(fixture_path(), L"", 1, 2, 5000);
    const auto first = call(7, "first");
    CHECK(broker.invoke_direct(first)["data"]["id"] == 7);
    CHECK_FALSE(broker.retire_connection(7, "other|key|generation"));
    CHECK(broker.retire_connection(7, first.session_identity));
    CHECK_FALSE(broker.retire_connection(7, first.session_identity));
    const auto next = call(7, "second");
    CHECK(broker.invoke_direct(next)["data"]["id"] == 7);
    CHECK_FALSE(broker.retire_connection(7, first.session_identity));
    CHECK(broker.retire_connection(7, next.session_identity));
}

TEST_CASE("Broker recreates a failed child for the next call without replaying the failed action", "[mcp][worker-broker]") {
    SessionWorkerBroker broker(fixture_path(), L"--exit-on-call", 1, 2, 5000);
    auto request = call(7, "first");
    try { (void)broker.invoke_direct(request); FAIL("first worker must exit"); }
    catch (const WorkerTransportError& error) { CHECK(error.code() == "OUTCOME_UNKNOWN"); }
    try { (void)broker.invoke_direct(request); FAIL("new worker fixture must also exit"); }
    catch (const WorkerTransportError& error) { CHECK(error.code() == "OUTCOME_UNKNOWN"); }
}

TEST_CASE("Broker rechecks policy at session dequeue", "[mcp][worker-broker]") {
    SessionWorkerBroker broker(fixture_path(), L"", 1, 2, 5000);
    auto denied = broker.submit(call(3, "one"), allow, [](const WorkerCall&) {
        return WorkerGateResult{false, "TOKEN_REVOKED", "revoked"};
    });
    CHECK(denied.get()["error"]["code"] == "TOKEN_REVOKED");
}

TEST_CASE("Different broker sessions run while one child waits", "[mcp][worker-broker]") {
    SessionWorkerBroker broker(fixture_path(), L"--sleep-first-call", 2, 2, 5000);
    auto slow = broker.submit(call(1, "one"), allow, allow);
    auto fast = broker.submit(call(2, "two"), allow, allow);
    CHECK(fast.wait_for(std::chrono::seconds(1)) == std::future_status::ready);
    CHECK(fast.get()["data"]["id"] == 2);
    CHECK(slow.get()["data"]["id"] == 1);
}

TEST_CASE("One broker session preserves FIFO order", "[mcp][worker-broker]") {
    SessionWorkerBroker broker(fixture_path(), L"--sleep-first-call", 1, 2, 5000);
    auto first_call = call(1, "one");
    auto second_call = first_call;
    second_call.id = 2;
    auto first = broker.submit(first_call, allow, allow);
    auto second = broker.submit(second_call, allow, allow);
    CHECK(second.wait_for(std::chrono::milliseconds(250)) == std::future_status::timeout);
    CHECK(first.get()["data"]["id"] == 1);
    CHECK(second.get()["data"]["id"] == 2);
}

TEST_CASE("Concurrent retirement cannot kill a newly submitted same-session call", "[mcp][worker-broker]") {
    SessionWorkerBroker broker(fixture_path(), L"", 1, 2, 5000);
    auto initial = call(1, "one");
    REQUIRE(broker.submit(initial, allow, allow).get()["data"]["id"] == 1);
    for (int id = 2; id < 10; ++id) {
        auto next = initial;
        next.id = id;
        std::thread retire([&] { (void)broker.retire_idle(initial.session_identity); });
        auto result = broker.submit(next, allow, allow);
        retire.join();
        REQUIRE(result.valid());
        CHECK(result.get()["data"]["id"] == id);
    }
}

TEST_CASE("Broker reports final outcome after an HTTP-style soft timeout", "[mcp][worker-broker]") {
    SessionWorkerBroker broker(fixture_path(), L"--sleep-first-call", 1, 2, 100);
    std::promise<json> final_promise;
    auto final = final_promise.get_future();
    auto response = broker.submit(call(1, "one"), allow, allow, nullptr,
                                  [&](const WorkerCall&, const json& result) { final_promise.set_value(result); });
    CHECK(response.get()["error"]["code"] == "CALL_TIMEOUT");
    REQUIRE(final.wait_for(std::chrono::seconds(4)) == std::future_status::ready);
    CHECK(final.get()["data"]["id"] == 1);
}
#endif
