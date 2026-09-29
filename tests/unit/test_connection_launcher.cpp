#include <catch2/catch_test_macros.hpp>
#include "include/connection_launcher.h"
#include "include/com/utf8.h"
#include <shellapi.h>
using namespace fairyfly::sap;

TEST_CASE("ConnectionLauncher matches only the requested connection", "[connection][launcher]") {
    REQUIRE(ConnectionLauncher::matches_connection_name("Bigfox", "Bigfox"));
    REQUIRE(ConnectionLauncher::matches_connection_name(" bigFOX ", "BIGfox"));
    REQUIRE_FALSE(ConnectionLauncher::matches_connection_name("Bigfox", "Production"));
    REQUIRE_FALSE(ConnectionLauncher::matches_connection_name("", "Bigfox"));
}

TEST_CASE("Fallback session identity survives GUI path reuse", "[connection][launcher]") {
    REQUIRE_FALSE(ConnectionLauncher::was_present_before_launch(
        "/app/con[0]/ses[0]", "old-system-session", "/app/con[0]/ses[0]",
        "new-system-session", false));
    REQUIRE(ConnectionLauncher::was_present_before_launch(
        "/app/con[0]/ses[0]", "same-system-session", "/app/con[1]/ses[0]",
        "same-system-session", false));
    REQUIRE(ConnectionLauncher::was_present_before_launch(
        "/app/con[0]/ses[0]", "", "/app/con[0]/ses[0]", "", false));
    REQUIRE_FALSE(ConnectionLauncher::was_present_before_launch(
        "/app/con[0]/ses[0]", "old-system-session", "/app/con[0]/ses[0]", "", false));
    REQUIRE_FALSE(ConnectionLauncher::was_present_before_launch(
        "/app/con[0]/ses[0]", "old-system-session", "/app/con[0]/ses[0]",
        "new-system-session", true));
    REQUIRE(ConnectionLauncher::was_present_before_launch(
        "/app/con[0]/ses[0]", "old-system-session", "/app/con[0]/ses[0]", "", true));
}

TEST_CASE("sapshcut arguments survive Windows command-line parsing", "[connection][launcher]") {
    CHECK(ConnectionLauncher::sapshcut_command_line(L"C:\\Program Files\\SAP\\sapshcut.exe", "Bigfox") ==
          L"\"C:\\Program Files\\SAP\\sapshcut.exe\" -sysname=Bigfox -maxgui");
    // Credentials must never reach the sapshcut command line (launch --login uses the scripting API).
    const auto sapshcut_line = ConnectionLauncher::sapshcut_command_line(L"C:\\sapshcut.exe", "Bigfox");
    CHECK(sapshcut_line.find(L"-pw") == std::wstring::npos);
    CHECK(sapshcut_line.find(L"-user") == std::wstring::npos);
    for (const std::string value : {"plain", "with space", "quote\"inside",
                                    "trailing\\", "slash\\\"quote", "p\xC3\xA4ss"}) {
        const std::wstring command = ConnectionLauncher::sapshcut_command_line(
            L"C:\\Program Files\\SAP\\sapshcut.exe", value);
        int count = 0;
        LPWSTR* parsed = CommandLineToArgvW(command.c_str(), &count);
        REQUIRE(parsed != nullptr);
        CHECK(count == 3);
        if (count == 3) {
            CHECK(std::wstring(parsed[0]) == L"C:\\Program Files\\SAP\\sapshcut.exe");
            CHECK(std::wstring(parsed[1]) ==
                  L"-sysname=" + fairyfly::com::utf8_to_wide(value));
            CHECK(std::wstring(parsed[2]) == L"-maxgui");
        }
        LocalFree(parsed);
    }
}

TEST_CASE("Shortcut security prompt stops session polling with a specific reason", "[connection][launcher]") {
    ConnectionLauncher launcher(nullptr, [] { return true; });
    const auto [connection, session] = launcher.wait_for_session("Bigfox", 5, {});
    CHECK(connection == nullptr);
    CHECK(session == nullptr);
    CHECK(launcher.wait_failure() == ConnectionLauncher::WaitFailure::SecurityPrompt);
}

