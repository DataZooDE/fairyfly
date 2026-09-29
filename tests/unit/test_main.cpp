#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <nlohmann/json.hpp>
#ifdef _WIN32
#include <windows.h>
#endif

int main(int argc, char* argv[]) {
#ifdef _WIN32
    // Keep unit-test CLI runs out of the real audit trail (audit tests set their own env).
    _putenv_s("FAIRYFLY_AUDIT", "0");
#endif
#ifdef _WIN32
    HDESK hDesk = OpenDesktopA("Default", 0, FALSE, GENERIC_ALL);
    if (hDesk) {
        SetThreadDesktop(hDesk);
    }
#endif
    return Catch::Session().run(argc, argv);
}

using json = nlohmann::json;

TEST_CASE("Basic JSON functionality", "[json]") {
    json response;
    response["status"] = "success";
    response["data"]["value"] = 42;

    REQUIRE(response["status"] == "success");
    REQUIRE(response["data"]["value"] == 42);
}

TEST_CASE("Error response structure", "[json]") {
    json response;
    response["status"] = "error";
    response["error"]["code"] = "TEST_ERROR";
    response["error"]["message"] = "Test error message";
    response["error"]["suggestions"] = json::array({"Suggestion 1", "Suggestion 2"});

    REQUIRE(response["status"] == "error");
    REQUIRE(response["error"]["code"] == "TEST_ERROR");
    REQUIRE(response["error"]["suggestions"].size() == 2);
}

// Placeholder tests - will be expanded when SAP components are implemented
TEST_CASE("Placeholder for SAP connection tests", "[sap][!mayfail]") {
    // These will be implemented in Phase 1.2
    REQUIRE(true);
}

TEST_CASE("Placeholder for element cache tests", "[automation][!mayfail]") {
    // These will be implemented in Phase 1.3
    REQUIRE(true);
}
