#include <catch2/catch_test_macros.hpp>
#include "include/cli_handler.h"
#include "include/credential_store.h"
#include "support/fake_engine.h"
#include "support/temp_dir.h"

using namespace fairyfly;
using namespace fairyfly::cli;
using fairyfly::test::FakeEngine;
using fairyfly::test::TempDir;

// The handler gets a fake engine: the real one would enumerate the SAP GUI of the machine the tests run on, over COM
// (on a desktop with SAP Logon open that call could block the whole run, depending on what earlier tests left in COM).
TEST_CASE("CommandHandler::handle_doctor diagnostics", "[cli][doctor]") {
    TempDir dir{"doctor"};
    auto fake = std::make_unique<FakeEngine>();
    fake->application_info = {{"total_connections", 1}, {"total_sessions", 2}};
    CommandHandler handler(std::make_unique<cred::InMemoryCredentialStore>(), std::move(fake),
                           std::make_unique<ConnectionManager>(dir.path().string()));

    SECTION("handle_doctor returns structured diagnostic checks") {
        Result result = handler.handle_doctor();
        REQUIRE(result.status == Result::Status::Success);
        REQUIRE(result.data.contains("checks"));
        REQUIRE(result.data["checks"].is_array());
        REQUIRE(result.data.contains("overall_health"));

        const auto& checks = result.data["checks"];
        REQUIRE(checks.size() >= 4);

        bool has_desktop = false;
        bool has_processes = false;
        bool has_registry = false;
        bool has_com = false;
        nlohmann::json sessions;

        for (const auto& check : checks) {
            REQUIRE(check.contains("name"));
            REQUIRE(check.contains("status"));
            REQUIRE(check.contains("message"));

            std::string name = check["name"];
            if (name == "desktop_context") has_desktop = true;
            if (name == "sapgui_processes") has_processes = true;
            if (name == "client_registry") has_registry = true;
            if (name == "com_engine") has_com = true;
            if (name == "active_sessions") sessions = check;
            if (name == "data_dir") CHECK(check["status"] == "pass");   // the test run's own data directory
        }

        REQUIRE(has_desktop);
        REQUIRE(has_processes);
        REQUIRE(has_registry);
        REQUIRE(has_com);
        // the sessions come from the injected engine, not from the SAP GUI of this machine
        REQUIRE(sessions.is_object());
        CHECK(sessions["status"] == "pass");
        CHECK(sessions["message"].get<std::string>().find("1 connections, 2 sessions") != std::string::npos);
    }
}
