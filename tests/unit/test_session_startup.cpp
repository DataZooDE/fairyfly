#include <catch2/catch_test_macros.hpp>
#include "include/session_startup.h"
#include "policy.hpp"
#include "include/startup_settings.h"
#include "include/session_preparation.h"
#include "include/startup_coordination.h"
#include "support/temp_dir.h"
#include <thread>
#include <map>

using fairyfly::startup::Policy;
using fairyfly::startup::check;

TEST_CASE("Automatic login attempts survive a new store instance and require manual recovery", "[startup][startup-coordination]") {
    fairyfly::test::TempDir temp{"startup-attempts"};
    fairyfly::startup::LoginAttempts first(temp.path());
    REQUIRE(first.begin("Bigfox").status == fairyfly::Result::Status::Success);
    fairyfly::startup::LoginAttempts next(temp.path());
    CHECK(next.begin("Bigfox").error.value("code", "") == "SAP_LOGIN_REQUIRED");
    CHECK(next.begin("Other").status == fairyfly::Result::Status::Success);
    REQUIRE(next.clear("Bigfox"));
    CHECK(first.begin("Bigfox").status == fairyfly::Result::Status::Success);
}

TEST_CASE("Automatic login refuses if its retry protection cannot be persisted", "[startup][startup-coordination]") {
    fairyfly::startup::LoginAttempts missing({});
    CHECK(missing.begin("Bigfox").status == fairyfly::Result::Status::Error);
}

TEST_CASE("Preparation lock is bounded and observes cancellation", "[startup][startup-coordination]") {
    auto first = fairyfly::startup::acquire_preparation_guard();
    REQUIRE(first.token);
    fairyfly::startup::PreparationGuard second;
    std::thread contender([&] {
        second = fairyfly::startup::acquire_preparation_guard({}, std::chrono::milliseconds(20));
    });
    contender.join();
    CHECK_FALSE(second.token);
    CHECK(second.result.error.value("code", "") == "SAP_STARTUP_BUSY");
    first.token.reset();
    CHECK(fairyfly::startup::acquire_preparation_guard().token);
    const auto cancelled = fairyfly::startup::acquire_preparation_guard([] { return true; });
    CHECK_FALSE(cancelled.token);
    CHECK(cancelled.result.error.value("code", "") == "CANCELLED");
}

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

namespace {
struct Preparation {
    fairyfly::Result listed = fairyfly::Result::ok({{"connections", fairyfly::json::array()}});
    fairyfly::Result login = fairyfly::Result::ok();
    bool ready = false, cancelled = false, attempted = false;
    int submissions = 0;
    std::vector<std::string> calls;
    fairyfly::startup::Operations operations() {
        return {[this](const std::string& tool, const fairyfly::json& args) {
            calls.push_back(tool);
            if (tool == "gui_session_list") return listed;
            if (tool == "gui_session_login") {
                CHECK(args["multiple_logon"] == "fail");
                if (attempted) return fairyfly::Result::fail("SAP_LOGIN_REQUIRED", "Complete login manually.");
                attempted = true;
                ++submissions;
                ready = login.status == fairyfly::Result::Status::Success;
                return login;
            }
            if (tool == "gui_session_launch") {
                CHECK(args["name"] == "Bigfox");
                CHECK_FALSE(args.value("login", false));
            }
            return fairyfly::Result::ok({{"connection_file_id", 7}});
        }, [this](int id) { CHECK(id == 7); return ready; }, [this] { return cancelled; }, {},
        [this] { attempted = false; }};
    }
    void session(const std::string& user) {
        listed = fairyfly::Result::ok({{"connections", fairyfly::json::array({
            {{"description", "Bigfox"}, {"sessions", fairyfly::json::array({
                {{"id", "/app/con[0]/ses[0]"}, {"user", user}, {"system", "A4H"}, {"client", "001"}}})}}})}});
    }
};
}

TEST_CASE("Session preparation reuses an authenticated session without startup permission", "[startup]") {
    Preparation fixture;
    fixture.session("DEVELOPER");
    fixture.ready = true;
    const auto result = fairyfly::startup::prepare({false, true, false, "Bigfox"}, fixture.operations());
    CHECK(result.status == fairyfly::Result::Status::Success);
    CHECK(fixture.calls == std::vector<std::string>{"gui_session_list", "gui_session_attach"});
}

TEST_CASE("Session preparation opens and logs in once then verifies readiness", "[startup]") {
    Preparation fixture;
    const auto result = fairyfly::startup::prepare({true, true, true, "Bigfox"}, fixture.operations());
    CHECK(result.status == fairyfly::Result::Status::Success);
    CHECK(fixture.calls == std::vector<std::string>{"gui_session_list", "gui_session_launch", "gui_session_login"});
    CHECK(result.data["connection_file_id"] == 7);
    CHECK(result.data.value("startup_changed_screen", false));
}

TEST_CASE("Session preparation allows SSO without looking up credentials", "[startup]") {
    Preparation fixture;
    fixture.ready = true; // the newly opened connection authenticated through SSO
    CHECK(fairyfly::startup::prepare({true, false, false, "Bigfox"}, fixture.operations()).status == fairyfly::Result::Status::Success);
    CHECK(fixture.calls == std::vector<std::string>{"gui_session_list", "gui_session_launch"});
}

TEST_CASE("Session preparation resumes a logged-out session without another launch", "[startup]") {
    Preparation fixture;
    fixture.session("");
    CHECK(fairyfly::startup::prepare({true, false, false, "Bigfox"}, fixture.operations()).status == fairyfly::Result::Status::Success);
    CHECK(fixture.calls == std::vector<std::string>{"gui_session_list", "gui_session_attach", "gui_session_login"});
}

TEST_CASE("Session preparation refuses before launch when either permission is missing", "[startup]") {
    for (const auto policy : {Policy{false, false, false, "Bigfox"}, Policy{true, true, false, "Bigfox"}}) {
        Preparation fixture;
        CHECK(fairyfly::startup::prepare(policy, fixture.operations()).status == fairyfly::Result::Status::Error);
        CHECK(fixture.calls == std::vector<std::string>{"gui_session_list"});
    }
}

TEST_CASE("Session preparation preserves login failure and never retries it", "[startup]") {
    Preparation fixture;
    fixture.login = fairyfly::Result::fail("LOGIN_FAILED", "Check credentials locally.");
    const auto result = fairyfly::startup::prepare({true, false, false, "Bigfox"}, fixture.operations());
    CHECK(result.error["code"] == "LOGIN_FAILED");
    CHECK(result.error["connection_open"] == true);
    CHECK(fixture.calls.size() == 3);
}

TEST_CASE("Cancelled session preparation performs no operations", "[startup]") {
    Preparation fixture;
    fixture.cancelled = true;
    CHECK(fairyfly::startup::prepare({true, false, false, "Bigfox"}, fixture.operations()).status == fairyfly::Result::Status::Error);
    CHECK(fixture.calls.empty());
}

TEST_CASE("Later requests do not retry a failed automatic login", "[startup]") {
    Preparation fixture;
    fixture.session("");
    fixture.login = fairyfly::Result::fail("LOGIN_FAILED", "Check credentials locally.");
    const Policy policy{true, false, false, "Bigfox"};
    REQUIRE(fairyfly::startup::prepare(policy, fixture.operations()).status == fairyfly::Result::Status::Error);
    fixture.calls.clear();
    const auto again = fairyfly::startup::prepare(policy, fixture.operations());
    CHECK(again.error.value("code", "") == "SAP_LOGIN_REQUIRED");
    CHECK(fixture.calls == std::vector<std::string>{"gui_session_list", "gui_session_attach", "gui_session_login"});
    CHECK(fixture.submissions == 1);
}

TEST_CASE("Cancellation after launch retains the opened connection and never logs in", "[startup]") {
    Preparation fixture;
    auto operations = fixture.operations();
    const auto invoke = operations.invoke;
    operations.invoke = [&](const std::string& name, const fairyfly::json& args) {
        const auto result = invoke(name, args);
        if (name == "gui_session_launch") fixture.cancelled = true;
        return result;
    };
    const auto result = fairyfly::startup::prepare({true, false, false, "Bigfox"}, operations);
    CHECK(result.error.value("code", "") == "CANCELLED");
    CHECK(result.error.value("connection_open", false));
    CHECK(result.error.value("connection_file_id", -1) == 7);
    CHECK(fixture.calls == std::vector<std::string>{"gui_session_list", "gui_session_launch"});
}

TEST_CASE("Startup checks the current local setting before mutation", "[startup]") {
    Preparation fixture;
    auto operations = fixture.operations();
    operations.current_policy = [] { return Policy{false, false, false, "Bigfox"}; };
    const auto result = fairyfly::startup::prepare({true, false, false, "Bigfox"}, operations);
    CHECK(result.status == fairyfly::Result::Status::Error);
    CHECK(fixture.calls == std::vector<std::string>{"gui_session_list"});
}

TEST_CASE("Incomplete discovery never starts or attaches a SAP session", "[startup][startup-discovery]") {
    Preparation fixture;
    fixture.session("DEVELOPER");
    fixture.ready = true;
    SECTION("connection enumeration failed") { fixture.listed.data["connection_enumeration_errors"] = 1; }
    SECTION("session enumeration failed") { fixture.listed.data["session_enumeration_errors"] = 1; }
    SECTION("malformed error count") { fixture.listed.data["session_enumeration_errors"] = "unknown"; }
    SECTION("unknown connection name") { fixture.listed.data["connections"][0]["description"] = nullptr; }
    SECTION("missing system on a reportedly logged-on session") {
        fixture.listed.data["connections"][0]["sessions"][0].erase("system");
    }
    const auto result = fairyfly::startup::prepare({true, false, false, "Bigfox"}, fixture.operations());
    CHECK(result.status == fairyfly::Result::Status::Error);
    CHECK(fixture.calls == std::vector<std::string>{"gui_session_list"});
}

TEST_CASE("Discovery preserves default selection and refuses ambiguity", "[startup][startup-discovery]") {
    Preparation fixture;
    fixture.session("DEVELOPER");
    fixture.ready = true;
    auto another = fixture.listed.data["connections"][0];
    SECTION("another matching session is ambiguous") {
        fixture.listed.data["connections"].push_back(another);
        const auto result = fairyfly::startup::prepare({true, false, false, "Bigfox"}, fixture.operations());
        CHECK(result.error.value("code", "") == "MULTIPLE_SESSIONS");
        CHECK(fixture.calls == std::vector<std::string>{"gui_session_list"});
    }
    SECTION("a different connection does not override the default") {
        another["description"] = "Other";
        fixture.listed.data["connections"].push_back(another);
        CHECK(fairyfly::startup::prepare({false, false, false, "Bigfox"}, fixture.operations()).status == fairyfly::Result::Status::Success);
        CHECK(fixture.calls == std::vector<std::string>{"gui_session_list", "gui_session_attach"});
    }
}

TEST_CASE("A successful login command must still prove authenticated readiness", "[startup][startup-discovery]") {
    Preparation fixture;
    auto operations = fixture.operations();
    operations.authenticated = [](int) { return false; };
    const auto result = fairyfly::startup::prepare({true, false, false, "Bigfox"}, operations);
    CHECK(result.error.value("code", "") == "SAP_LOGIN_REQUIRED");
    CHECK(result.error.value("connection_file_id", -1) == 7);
    CHECK(fixture.submissions == 1);
    CHECK(fixture.attempted);
}

TEST_CASE("Changes after launch prevent automatic login", "[startup][startup-discovery]") {
    Preparation fixture;
    auto operations = fixture.operations();
    bool changed_default = false;
    SECTION("startup disabled") {}
    SECTION("default changed") { changed_default = true; }
    operations.current_policy = [&] {
        const bool opened = fixture.calls.size() > 1;
        return Policy{!opened || changed_default, false, false, opened && changed_default ? "Other" : "Bigfox"};
    };
    const auto result = fairyfly::startup::prepare({true, false, false, "Bigfox"}, operations);
    CHECK(result.error.value("code", "") == (changed_default ? "SAP_STARTUP_CONFIG_CHANGED" : "SAP_AUTOSTART_DISABLED"));
    CHECK(result.error.value("connection_file_id", -1) == 7);
    CHECK(fixture.calls == std::vector<std::string>{"gui_session_list", "gui_session_launch"});
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
