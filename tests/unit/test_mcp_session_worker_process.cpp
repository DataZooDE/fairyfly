#include <catch2/catch_test_macros.hpp>

#include "include/mcp/session_worker_process.h"

#ifdef _WIN32
#include <filesystem>
#include <future>
#include <windows.h>

using namespace fairyfly::mcp;

namespace {
std::wstring fixture_path() {
    wchar_t path[MAX_PATH]{};
    REQUIRE(GetModuleFileNameW(nullptr, path, MAX_PATH) > 0);
    return (std::filesystem::path(path).parent_path() / L"mcp_worker_fixture.exe").wstring();
}
WorkerCall call(int id) {
    WorkerCall c;
    c.id = id;
    c.connection = 7;
    c.session_identity = "/app/con[0]/ses[0]|server-key|generation";
    c.owner_identity = "A4H/001/OWNER";
    c.argv = {"screen", "read", "--connection", "7"};
    return c;
}
}

TEST_CASE("Private worker process serves repeated session calls", "[mcp][worker-process]") {
    SessionWorkerProcess worker(fixture_path(), L"");
    CHECK(worker.invoke(call(1))["data"]["id"] == 1);
    CHECK(worker.invoke(call(2))["data"]["id"] == 2);
}

TEST_CASE("Private worker process returns a connection probe before binding", "[mcp][worker-process]") {
    SessionWorkerProcess worker(fixture_path(), L"");
    WorkerCall probe;
    probe.id = 9;
    probe.connection = 7;
    probe.probe = true;
    CHECK(worker.invoke(probe)["data"]["connection_id"] == 7);
    CHECK(worker.invoke(call(10))["data"]["id"] == 10);
}

TEST_CASE("Private worker rechecks admission after waiting for its pipe", "[mcp][worker-process]") {
    SessionWorkerProcess worker(fixture_path(), L"--sleep-first-call");
    auto first = std::async(std::launch::async, [&] { return worker.invoke(call(1)); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    bool gate_ran = false;
    try {
        (void)worker.invoke(call(2), [&] {
            gate_ran = true;
            throw WorkerTransportError("TOKEN_CHANGED", "revoked before dispatch");
        });
        FAIL("revoked action must not reach the worker");
    } catch (const WorkerTransportError& error) {
        CHECK(error.code() == "TOKEN_CHANGED");
    }
    CHECK(gate_ran);
    CHECK(first.get()["data"]["id"] == 1);
}

TEST_CASE("Worker death after submission is an unknown outcome", "[mcp][worker-process]") {
    SessionWorkerProcess worker(fixture_path(), L"--exit-on-call");
    try {
        (void)worker.invoke(call(3));
        FAIL("worker exit must fail");
    } catch (const WorkerTransportError& error) {
        CHECK(error.code() == "OUTCOME_UNKNOWN");
    }
}

TEST_CASE("A worker write side effect is not replayed after its response is lost", "[mcp][worker-process]") {
    const auto marker = std::filesystem::temp_directory_path() /
        (L"fairyfly_worker_write_" + std::to_wstring(GetCurrentProcessId()) + L"_" +
         std::to_wstring(GetTickCount64()) + L".marker");
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ignored; std::filesystem::remove(path, ignored); }
    } cleanup{marker};
    SessionWorkerProcess worker(fixture_path(), L"--effect-then-exit \"" + marker.wstring() + L"\"");
    WorkerCall write = call(41);
    write.argv = {"element", "fill", "wnd[0]/usr/txtTEST", "value", "--connection", "7"};
    try {
        (void)worker.invoke(write);
        FAIL("a lost write response must have an unknown outcome");
    } catch (const WorkerTransportError& error) {
        CHECK(error.code() == "OUTCOME_UNKNOWN");
    }
    REQUIRE(std::filesystem::exists(marker));
    CHECK(std::filesystem::file_size(marker) == 1);
}

TEST_CASE("Hung worker can be terminated out of band", "[mcp][worker-process]") {
    SessionWorkerProcess worker(fixture_path(), L"--hang-on-call");
    auto result = std::async(std::launch::async, [&] {
        try { (void)worker.invoke(call(4)); }
        catch (const WorkerTransportError& error) { return error.code(); }
        return std::string("NO_ERROR");
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    worker.terminate();
    CHECK(result.wait_for(std::chrono::seconds(2)) == std::future_status::ready);
    CHECK(result.get() == "OUTCOME_UNKNOWN");
}

TEST_CASE("Production worker enforces inherited read-only cap before SAP access", "[mcp][worker-process]") {
    wchar_t previous[128]{};
    const DWORD length = GetEnvironmentVariableW(L"FAIRYFLY_READ_ONLY", previous, 128);
    REQUIRE(length < 128);
    struct Restore {
        DWORD length;
        wchar_t* value;
        ~Restore() { SetEnvironmentVariableW(L"FAIRYFLY_READ_ONLY", length ? value : nullptr); }
    } restore{length, previous};
    REQUIRE(SetEnvironmentVariableW(L"FAIRYFLY_READ_ONLY", L"1"));
    auto request = call(5);
    request.argv = {"element", "fill", "--connection", "7", "--id", "field", "--value", "x"};
    request.selection_input = true;
    request.selection_program = "SAPMSSY0";
    request.selection_screen = "0120";
    SessionWorkerProcess worker((std::filesystem::path(fixture_path()).parent_path() / L"fairyfly.exe").wstring());
    CHECK(worker.invoke(request)["error"]["code"] == "READ_ONLY");
}
#endif
