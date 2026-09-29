#include <catch2/catch_test_macros.hpp>
#include "include/login_flow.h"

#include <sstream>

using fairyfly::LoginCredentials;

TEST_CASE("Login credentials parse the existing colon file without losing password punctuation", "[login]") {
    std::istringstream input("Connection: Bigfox\nUsername: DEVELOPER\nPassword: p:a:s:s\nSystem ID: 001\n");
    const auto credentials = fairyfly::parse_login_credentials(input);
    REQUIRE(credentials.username == "DEVELOPER");
    REQUIRE(credentials.password == "p:a:s:s");
    REQUIRE(credentials.client == "001");
    REQUIRE(credentials.language == "EN");
}

TEST_CASE("Login credentials reject missing password and malformed client", "[login]") {
    std::istringstream missing("Username: DEVELOPER\nSystem ID: 001\n");
    REQUIRE_THROWS_AS(fairyfly::parse_login_credentials(missing), std::invalid_argument);
    std::istringstream bad_client("Username: DEVELOPER\nPassword: secret\nSystem ID: 1A1\n");
    REQUIRE_THROWS_AS(fairyfly::parse_login_credentials(bad_client), std::invalid_argument);
}

TEST_CASE("Piped login credentials accept a UTF-8 BOM and a new password", "[login]") {
    std::istringstream input("\xEF\xBB\xBFUsername: ZFFTEST\nPassword: old:secret\nNew Password: new:secret\nSystem ID: 001\n");
    const auto credentials = fairyfly::parse_login_credentials(input);
    REQUIRE(credentials.username == "ZFFTEST");
    REQUIRE(credentials.password == "old:secret");
    REQUIRE(credentials.new_password == "new:secret");
}

TEST_CASE("SAP logon completion requires leaving S000 and its password field", "[login]") {
    nlohmann::json login = {{"transaction", "S000"}, {"elements", nlohmann::json::array({{{"id", "/app/con[0]/ses[0]/wnd[0]/usr/pwdRSYST-BCODE"}}})}};
    nlohmann::json menu = {{"transaction", "SESSION_MANAGER"}, {"elements", nlohmann::json::array()}};
    REQUIRE(fairyfly::is_sap_logon_screen(login));
    REQUIRE_FALSE(fairyfly::sap_logon_completed(login));
    REQUIRE_FALSE(fairyfly::is_sap_logon_screen(menu));
    REQUIRE(fairyfly::sap_logon_completed(menu));
}

TEST_CASE("Authenticated SAP user comparison ignores letter case but requires exact identity", "[login]") {
    REQUIRE(fairyfly::sap_user_matches("zfftest", "ZFFTEST"));
    REQUIRE_FALSE(fairyfly::sap_user_matches("DEVELOPER", "ZFFTEST"));
    REQUIRE_FALSE(fairyfly::sap_user_matches("", "ZFFTEST"));
}

// ---- multiple-logon dialog ----
#include "include/cli_entry.h"
#include <iostream>
#include <vector>

namespace {
using fairyfly::MultipleLogonPlan;
using Action = MultipleLogonPlan::Action;

std::string run_cli_text(std::vector<std::string> args, int& code) {
    std::vector<char*> argv;
    for (auto& a : args) argv.push_back(a.data());
    std::ostringstream out, err;
    auto* old_out = std::cout.rdbuf(out.rdbuf());
    auto* old_err = std::cerr.rdbuf(err.rdbuf());
    code = fairyfly::cli::run_cli(static_cast<int>(argv.size()), argv.data());
    std::cout.rdbuf(old_out);
    std::cerr.rdbuf(old_err);
    return out.str() + err.str();
}
} // namespace

TEST_CASE("plan_multiple_logon selects the right radio per choice", "[login][multiple-logon]") {
    for (bool read_only : {false, true}) {
        auto fail = fairyfly::plan_multiple_logon("fail", true, read_only);
        REQUIRE(fail.action == Action::Fail);
        REQUIRE(!fail.error_code);
        auto keep = fairyfly::plan_multiple_logon("keep", true, read_only);
        REQUIRE(keep.action == Action::Select);
        REQUIRE(keep.radio_suffix == "usr/radMULTI_LOGON_OPT2");
        auto terminate = fairyfly::plan_multiple_logon("terminate", true, read_only);
        REQUIRE(terminate.action == Action::Select);
        REQUIRE(terminate.radio_suffix == "usr/radMULTI_LOGON_OPT3");
    }
    auto end = fairyfly::plan_multiple_logon("end", true, false);
    REQUIRE(end.action == Action::Select);
    REQUIRE(end.radio_suffix == "usr/radMULTI_LOGON_OPT1");
    REQUIRE(end.choice == "end");
}

TEST_CASE("plan_multiple_logon refuses end under read-only", "[login][multiple-logon]") {
    auto end = fairyfly::plan_multiple_logon("end", true, true);
    REQUIRE(end.action == Action::Refuse);
    REQUIRE(end.error_code == "READ_ONLY_REFUSED");
    REQUIRE(end.radio_suffix.empty());
}

TEST_CASE("plan_multiple_logon does nothing without the dialog and rejects unknown values", "[login][multiple-logon]") {
    for (const auto* choice : {"fail", "keep", "end", "terminate"})
        for (bool read_only : {false, true})
            REQUIRE(fairyfly::plan_multiple_logon(choice, false, read_only).action == Action::None);
    for (const auto* bad : {"bogus", "", "KEEP", "Fail", "ter", "e"})
        for (bool present : {false, true}) {
            auto plan = fairyfly::plan_multiple_logon(bad, present, false);
            REQUIRE(plan.action == Action::Refuse);
            REQUIRE(plan.error_code == "INVALID_ARGUMENT");
        }
}

TEST_CASE("multiple-logon fail error and success annotation shapes", "[login][multiple-logon]") {
    auto error = fairyfly::make_multiple_logon_fail_error("User DEVELOPER is already logged on in client 001",
                                                          "(terminal T1, since 10:00)");
    REQUIRE(error.at("code") == "LOGON_NOT_COMPLETED");
    REQUIRE(error.at("reason") == "multiple_logon_dialog");
    const auto hint = error.at("hint").get<std::string>();
    REQUIRE(hint.find("--multiple-logon keep") != std::string::npos);
    REQUIRE(hint.find("--multiple-logon end") != std::string::npos);
    REQUIRE(hint.find("--multiple-logon terminate") != std::string::npos);
    REQUIRE(error.at("dialog").at("user") == "User DEVELOPER is already logged on in client 001");
    REQUIRE(error.at("dialog").at("terminal") == "(terminal T1, since 10:00)");

    // The real dialog pads its fields with blanks and may omit the terminal line entirely.
    auto padded = fairyfly::make_multiple_logon_fail_error(
        "User DEVELOPER is already logged on in client 001                               ", "");
    REQUIRE(padded.at("dialog").at("user") == "User DEVELOPER is already logged on in client 001");
    REQUIRE(padded.at("dialog").at("terminal") == "");

    auto keep = fairyfly::make_multiple_logon_annotation("keep");
    REQUIRE(keep.at("detected") == true);
    REQUIRE(keep.at("choice") == "keep");
    REQUIRE(!keep.contains("warning"));
    auto end = fairyfly::make_multiple_logon_annotation("end");
    REQUIRE(end.at("warning") == "other logons of this user were ended");
}

TEST_CASE("CLI validates --multiple-logon without touching SAP", "[login][multiple-logon]") {
    int code = 0;
    auto out = run_cli_text({"fairyfly", "--no-audit", "login", "--multiple-logon", "bogus"}, code);
    REQUIRE(code != 0);
    REQUIRE(out.find("bogus") != std::string::npos);
    out = run_cli_text({"fairyfly", "--no-audit", "login", "--multiple-logon", "END"}, code);
    REQUIRE(code != 0);
    out = run_cli_text({"fairyfly", "--no-audit", "launch", "SomeConn", "--multiple-logon", "keep"}, code);
    REQUIRE(code != 0);
    REQUIRE(out.find("INVALID_ARGUMENT") != std::string::npos);
}
