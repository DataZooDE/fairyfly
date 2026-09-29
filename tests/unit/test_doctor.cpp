#include <catch2/catch_test_macros.hpp>
#include "include/cli_handler.h"

using namespace fairyfly;
using namespace fairyfly::cli;

TEST_CASE("CommandHandler::handle_doctor diagnostics", "[cli][doctor]") {
    CommandHandler handler;

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

        for (const auto& check : checks) {
            REQUIRE(check.contains("name"));
            REQUIRE(check.contains("status"));
            REQUIRE(check.contains("message"));

            std::string name = check["name"];
            if (name == "desktop_context") has_desktop = true;
            if (name == "sapgui_processes") has_processes = true;
            if (name == "client_registry") has_registry = true;
            if (name == "com_engine") has_com = true;
        }

        REQUIRE(has_desktop);
        REQUIRE(has_processes);
        REQUIRE(has_registry);
        REQUIRE(has_com);
    }
}
