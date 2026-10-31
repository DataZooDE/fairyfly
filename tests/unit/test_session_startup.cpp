#include <catch2/catch_test_macros.hpp>
#include "include/session_startup.h"
#include "policy.hpp"
#include "include/startup_settings.h"
#include <map>

using fairyfly::startup::Policy;
using fairyfly::startup::check;

namespace {
struct Settings : fairyfly::tray::TraySettings {
    std::map<std::string, std::string> values;
    bool writable = true;
    std::optional<std::string> get(const std::string& key) override {
        const auto found = values.find(key);
        return found == values.end() ? std::nullopt : std::optional<std::string>(found->second);
    }
    bool set(const std::string& key, const std::string& value) override {
        if (!writable) return false;
        values[key] = value;
        return true;
    }
};
}

TEST_CASE("Startup settings use checkbox for trays and explicit flag for headless runs", "[startup]") {
    using namespace fairyfly;
    Settings settings;
    settings.values[tray::kAllowSapStartupSetting] = "1";
    settings.values[tray::kSapDefaultConnectionSetting] = "Bigfox";
    CHECK(startup::read_local(settings, true, false).local_enabled);
    CHECK_FALSE(startup::read_local(settings, false, false).local_enabled);
    CHECK(startup::read_local(settings, false, true).local_enabled);
    settings.values[tray::kAllowSapStartupSetting] = "0";
    CHECK_FALSE(startup::read_local(settings, true, true).local_enabled);
    CHECK(startup::read_local(settings, false, true).default_connection == "Bigfox");
}

TEST_CASE("Default SAP connection is saved locally without enabling startup", "[startup]") {
    using namespace fairyfly;
    Settings settings;
    REQUIRE(startup::configure_default(settings, "Bigfox").status == Result::Status::Success);
    CHECK(settings.get(tray::kSapDefaultConnectionSetting) == "Bigfox");
    CHECK_FALSE(startup::read_local(settings, true, false).local_enabled);
    CHECK(startup::configure_default(settings, "  ").status == Result::Status::Error);
    CHECK(settings.get(tray::kSapDefaultConnectionSetting) == "Bigfox");
    settings.writable = false;
    CHECK(startup::configure_default(settings, "Other").status == Result::Status::Error);
}

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
