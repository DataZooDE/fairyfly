#include <catch2/catch_test_macros.hpp>
#include "include/session_startup.h"
#include "policy.hpp"

using fairyfly::startup::Policy;
using fairyfly::startup::check;

TEST_CASE("SAP startup requires local opt in with actionable instructions", "[startup]") {
    const auto result = check(Policy{false, false, false, "Bigfox"});
    REQUIRE(result.has_value());
    CHECK(result->error["code"] == "SAP_AUTOSTART_DISABLED");
    CHECK(result->error.dump().find("Allow automatic SAP startup") != std::string::npos);
    CHECK(result->error.dump().find("--allow-sap-startup") != std::string::npos);
}

TEST_CASE("Link startup needs gateway permission even with local opt in", "[startup]") {
    const auto result = check(Policy{true, true, false, "Bigfox"});
    REQUIRE(result.has_value());
    CHECK(result->error["code"] == "SAP_AUTOSTART_ADMIN_DISABLED");
    CHECK(result->error.dump().find("administrator") != std::string::npos);
}

TEST_CASE("SAP startup requires a configured local default", "[startup]") {
    const auto result = check(Policy{true, false, false, ""});
    REQUIRE(result.has_value());
    CHECK(result->error["code"] == "SAP_AUTOSTART_DEFAULT_MISSING");
}

TEST_CASE("Local startup does not depend on gateway permission", "[startup]") {
    CHECK_FALSE(check(Policy{true, false, false, "Bigfox"}).has_value());
    CHECK_FALSE(check(Policy{true, true, true, "Bigfox"}).has_value());
    CHECK(check(Policy{false, true, true, "Bigfox"}).has_value());
}

TEST_CASE("Gateway startup grant round trips independently of writes", "[startup][link]") {
    using ffy::policy::Grants;
    const auto legacy = Grants::from_json({{"allow_write", true}}).to_json();
    REQUIRE(legacy.contains("allow_sap_startup"));
    CHECK(legacy["allow_sap_startup"] == false);
    const auto granted = Grants::from_json({{"allow_write", false}, {"allow_sap_startup", true}}).to_json();
    CHECK(granted["allow_sap_startup"] == true);
    CHECK(granted["allow_write"] == false);
}

TEST_CASE("Gateway startup policy can be explicitly granted", "[startup][link]") {
    const auto policy = ffy::policy::Engine::load("default:\n  allow_sap_startup: true\n");
    CHECK(policy.grants({"user@example.com", "chatgpt", {}}).to_json().value("allow_sap_startup", false));
}
