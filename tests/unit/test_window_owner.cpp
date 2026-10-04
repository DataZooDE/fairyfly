#include <catch2/catch_test_macros.hpp>

#ifdef _WIN32
#include <windows.h>

#include "include/system/window_owner.h"

TEST_CASE("SAP COM Long window handles preserve Win64 sign extension", "[window-owner][auth]") {
    CHECK(fairyfly::system::sap_com_long_to_window_handle(0x7fffffff) ==
          static_cast<std::uintptr_t>(0x7fffffff));
    CHECK(fairyfly::system::sap_com_long_to_window_handle(static_cast<std::int32_t>(0x80000000u)) ==
          static_cast<std::uintptr_t>(static_cast<std::intptr_t>(static_cast<std::int32_t>(0x80000000u))));
    CHECK(fairyfly::system::sap_com_long_to_window_handle(-1) ==
          static_cast<std::uintptr_t>(static_cast<std::intptr_t>(-1)));
}

TEST_CASE("Windows owner guard accepts only live windows of this logon", "[window-owner][auth]") {
    CHECK(fairyfly::system::process_owned_by_current_logon(GetCurrentProcessId()));
    CHECK_FALSE(fairyfly::system::process_owned_by_current_logon(0));

    HWND own = CreateWindowExW(0, L"STATIC", L"owner guard test", 0,
                               0, 0, 1, 1, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
    REQUIRE(own != nullptr);
    CHECK(fairyfly::system::window_owned_by_current_logon(reinterpret_cast<std::uintptr_t>(own)));
    REQUIRE(DestroyWindow(own));
    CHECK_FALSE(fairyfly::system::window_owned_by_current_logon(reinterpret_cast<std::uintptr_t>(own)));
    CHECK_FALSE(fairyfly::system::window_owned_by_current_logon(0));
}
#endif
