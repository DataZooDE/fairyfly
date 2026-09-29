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
