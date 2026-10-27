#include <catch2/catch_session.hpp>
#include <catch2/catch_test_macros.hpp>
#include <catch2/reporters/catch_reporter_event_listener.hpp>
#include <catch2/reporters/catch_reporter_registrars.hpp>
#include <nlohmann/json.hpp>
#include "support/fake_dispatch.h"
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

// The DISPID cache is process-wide and keyed by SAP type: a fake GuiTextField without Changeable would otherwise
// leave a cached miss that turns the next test's read-only GuiTextField writable (order-dependent failures).
class DispatchCacheResetListener : public Catch::EventListenerBase {
public:
    using Catch::EventListenerBase::EventListenerBase;
    void testCaseStarting(const Catch::TestCaseInfo&) override { fairyfly::test::DispatchCacheAccess::clear_dispid_cache(); }
};
CATCH_REGISTER_LISTENER(DispatchCacheResetListener)

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

