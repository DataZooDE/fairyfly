#include <catch2/catch_test_macros.hpp>

#include <stdexcept>
#include <sstream>

#include "include/mcp/sap_gui_hosting.h"
#include "include/mcp/session_worker_protocol.h"

#ifdef _WIN32
#include "include/com/wrapper.h"
#endif

using namespace fairyfly::mcp;

TEST_CASE("Owner-mode HTTP endpoints refuse the embedded SAP GUI fallback", "[mcp][worker-protocol][sap-logon]") {
    // Session workers attach through the running object table; windows of an embedded SapGui.ScriptingCtrl live in
    // the tray process and could never be driven.
    CHECK_FALSE(embedded_sap_gui_allowed(true, {"A4H/001/DEVELOPER"}));
    CHECK_FALSE(embedded_sap_gui_allowed(true, {"A4H/001/ALICE", "A4H/001/BOB"}));
    CHECK(embedded_sap_gui_allowed(true, {}));                      // plain HTTP: one process drives everything
    CHECK(embedded_sap_gui_allowed(false, {}));                     // stdio
    CHECK(embedded_sap_gui_allowed(false, {"A4H/001/DEVELOPER"}));  // owner identities only apply to HTTP
}

#ifdef _WIN32
TEST_CASE("The embedded SAP GUI fallback switch is process-wide and defaults to allowed", "[mcp][sap-logon]") {
    using fairyfly::sap::ComGuiApplication;
    CHECK(ComGuiApplication::embedded_fallback_allowed());
    ComGuiApplication::set_embedded_fallback_allowed(false);
    CHECK_FALSE(ComGuiApplication::embedded_fallback_allowed());
    ComGuiApplication::set_embedded_fallback_allowed(true);   // restore for the other tests in this process
    CHECK(ComGuiApplication::embedded_fallback_allowed());
}
#endif

namespace {
json valid_call() {
    return {{"version", 1}, {"id", 17},
            {"session", {{"connection", 7}, {"identity", "/app/con[0]/ses[0]|server-key|generation"},
                         {"owner", "A4H/001/OWNER"}}},
            {"argv", json::array({"screen", "read", "--connection", "7", "--no-tabs"})},
            {"read_only", true}};
}
}

TEST_CASE("Session worker accepts a bounded session-bound screen call", "[mcp][worker-protocol]") {
    const auto call = decode_worker_call(valid_call().dump());
    CHECK(call.id == 17);
    CHECK(call.connection == 7);
    CHECK(call.session_identity == "/app/con[0]/ses[0]|server-key|generation");
    CHECK(call.owner_identity == "A4H/001/OWNER");
    CHECK(call.argv == std::vector<std::string>{"screen", "read", "--connection", "7", "--no-tabs"});
    CHECK(call.read_only);
}

TEST_CASE("Session worker carries and validates a screen precondition", "[mcp][worker-protocol]") {
    WorkerCall call = decode_worker_call(valid_call().dump());
    call.expected_screen_guard = std::string(64, 'a');
    CHECK(decode_worker_call(encode_worker_call(call)).expected_screen_guard == call.expected_screen_guard);
    auto frame = json::parse(encode_worker_call(call));
    frame["expected_screen_guard"] = "short";
    CHECK_THROWS_AS(decode_worker_call(frame.dump()), std::invalid_argument);
    frame = json::parse(encode_worker_call(call));
    frame["expected_screen_guard"] = std::string(64, 'z');
    CHECK_THROWS_AS(decode_worker_call(frame.dump()), std::invalid_argument);
    WorkerCall probe;
    probe.probe = true;
    probe.connection = 7;
    frame = json::parse(encode_worker_call(probe));
    frame["expected_screen_guard"] = std::string(64, 'a');
    CHECK_THROWS_AS(decode_worker_call(frame.dump()), std::invalid_argument);
}

TEST_CASE("Session worker accepts disconnect only for its bound connection", "[mcp][worker-protocol]") {
    auto call = valid_call();
    call["argv"] = json::array({"session", "disconnect", "--connection", "7", "--close-session"});
    CHECK(decode_worker_call(call.dump()).argv ==
          std::vector<std::string>{"session", "disconnect", "--connection", "7", "--close-session"});
    call["argv"][3] = "8";
    CHECK_THROWS_AS(decode_worker_call(call.dump()), std::invalid_argument);
}

TEST_CASE("Session worker probe is read-only and cannot carry a command", "[mcp][worker-protocol]") {
    WorkerCall request;
    request.id = 18;
    request.connection = 7;
    request.probe = true;
    const auto decoded = decode_worker_call(encode_worker_call(request));
    CHECK(decoded.probe);
    CHECK(decoded.connection == 7);
    CHECK(decoded.argv.empty());
    request.connection = -1; // resolve only when exactly one saved session exists
    const auto automatic = decode_worker_call(encode_worker_call(request));
    CHECK(automatic.probe);
    CHECK(automatic.connection == -1);
    request.connection = 7;
    auto frame = json::parse(encode_worker_call(request));
    frame["argv"] = json::array({"session", "disconnect"});
    CHECK_THROWS_AS(decode_worker_call(frame.dump()), std::invalid_argument);
    frame = json::parse(encode_worker_call(request));
    frame["read_only"] = false;
    CHECK_THROWS_AS(decode_worker_call(frame.dump()), std::invalid_argument);
    frame = json::parse(encode_worker_call(request));
    frame["session"]["identity"] = "/app/con[0]/ses[0]|key|generation";
    CHECK_THROWS_AS(decode_worker_call(frame.dump()), std::invalid_argument);
}

TEST_CASE("Session worker discovery is an isolated read-only control request", "[mcp][worker-protocol]") {
    WorkerCall request;
    request.id = 21;
    request.enumerate = true;
    const auto decoded = decode_worker_call(encode_worker_call(request));
    CHECK(decoded.enumerate);
    CHECK(decoded.connection == -1);
    CHECK(decoded.argv.empty());
    auto frame = json::parse(encode_worker_call(request));
    frame["session"]["connection"] = 7;
    CHECK_THROWS_AS(decode_worker_call(frame.dump()), std::invalid_argument);
    frame = json::parse(encode_worker_call(request));
    frame["argv"] = json::array({"session", "disconnect"});
    CHECK_THROWS_AS(decode_worker_call(frame.dump()), std::invalid_argument);
    frame = json::parse(encode_worker_call(request));
    frame["read_only"] = false;
    CHECK_THROWS_AS(decode_worker_call(frame.dump()), std::invalid_argument);
    frame = json::parse(encode_worker_call(request));
    frame["probe"] = true;
    CHECK_THROWS_AS(decode_worker_call(frame.dump()), std::invalid_argument);
}

TEST_CASE("Session worker rejects unbound, privileged and oversized calls", "[mcp][worker-protocol]") {
    auto call = valid_call();
    call["argv"] = json::array({"screen", "read", "--connection", "8"});
    CHECK_THROWS_AS(decode_worker_call(call.dump()), std::invalid_argument);
    call = valid_call();
    call["argv"] = json::array({"session", "launch", "PRD", "--connection", "7"});
    CHECK_THROWS_AS(decode_worker_call(call.dump()), std::invalid_argument);
    call = valid_call();
    call["session"]["identity"] = "";
    CHECK_THROWS_AS(decode_worker_call(call.dump()), std::invalid_argument);
    call = valid_call();
    call["version"] = 2;
    CHECK_THROWS_AS(decode_worker_call(call.dump()), std::invalid_argument);
    call = valid_call();
    call["version"] = "1";
    CHECK_THROWS_AS(decode_worker_call(call.dump()), std::invalid_argument);
    call = valid_call();
    call["id"] = 9223372036854775807LL;
    CHECK_THROWS_AS(decode_worker_call(call.dump()), std::invalid_argument);
    call = valid_call();
    call["session"]["connection"] = 9223372036854775807LL;
    CHECK_THROWS_AS(decode_worker_call(call.dump()), std::invalid_argument);
    CHECK_THROWS_AS(decode_worker_call(std::string(65537, 'x')), std::invalid_argument);
}

TEST_CASE("Session worker loop returns one result per bounded private request", "[mcp][worker-protocol]") {
    std::istringstream input(valid_call().dump() + "\n");
    std::ostringstream output;
    int calls = 0;
    const int exit = run_worker_loop(input, output, [&](const WorkerCall& call) {
        ++calls;
        fairyfly::Result result;
        result.status = fairyfly::Result::Status::Success;
        result.data = {{"connection_id", call.connection}};
        return result;
    });
    REQUIRE(exit == 0);
    REQUIRE(calls == 1);
    const auto response = json::parse(output.str());
    CHECK(response["version"] == 1);
    CHECK(response["id"] == 17);
    CHECK(response["result"]["status"] == "success");
    CHECK(response["result"]["data"]["connection_id"] == 7);
}

TEST_CASE("Unhandled worker exception retires with unknown outcome", "[mcp][worker-protocol]") {
    std::istringstream input(valid_call().dump() + "\n" + valid_call().dump() + "\n");
    std::ostringstream output;
    int calls = 0;
    const int exit = run_worker_loop(input, output, [&](const WorkerCall&) -> fairyfly::Result {
        ++calls;
        throw std::runtime_error("side effect may have happened");
    });
    CHECK(exit != 0);
    CHECK(calls == 1);
    const auto response = json::parse(output.str());
    CHECK(response["result"]["error"]["code"] == "OUTCOME_UNKNOWN");
}

TEST_CASE("Read-only discovery failure does not claim a SAP action may have run", "[mcp][worker-protocol]") {
    WorkerCall discovery;
    discovery.enumerate = true;
    std::istringstream input(encode_worker_call(discovery) + "\n");
    std::ostringstream output;
    const int exit = run_worker_loop(input, output, [](const WorkerCall&) -> fairyfly::Result {
        throw std::runtime_error("SAP enumeration failed");
    });
    CHECK(exit == 0);
    CHECK(json::parse(output.str())["result"]["error"]["code"] == "WORKER_UNAVAILABLE");
}
