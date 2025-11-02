#include <catch2/catch_test_macros.hpp>
#include "include/screen_reader.h"
#include "include/com/wrapper.h"

using namespace fairyfly;
using namespace fairyfly::sap;

TEST_CASE("ScreenReader - Read with no session", "[screen][reader]") {
    ScreenReader reader(nullptr);

    SECTION("Returns error when no session") {
        Result result = reader.read(true);
        REQUIRE(result.status == Result::Status::Error);
        REQUIRE(result.error["code"] == "NO_SESSION");
    }

    SECTION("Read with tabs returns error when no session") {
        Result result = reader.read_with_tabs();
        REQUIRE(result.status == Result::Status::Error);
        REQUIRE(result.error["code"] == "NO_SESSION");
    }
}

