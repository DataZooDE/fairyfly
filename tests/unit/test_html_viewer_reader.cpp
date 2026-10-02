// HTML viewer UIA read: bounded retry decision, fast empty paths, browser-window gate helper.
#include <catch2/catch_test_macros.hpp>
#include "include/html_viewer_reader.h"
#include <chrono>
#include <cstdint>
#include <windows.h>

using namespace fairyfly::sap;

TEST_CASE("HTML viewer retry decision retries only an empty document within budget",
          "[html_viewer][perf]") {
    const HtmlReadBudget budget{300, 50};
    CHECK(retry_decision(HtmlReadOutcome::DocumentEmpty, 0, budget));
    CHECK(retry_decision(HtmlReadOutcome::DocumentEmpty, 249, budget));
    CHECK_FALSE(retry_decision(HtmlReadOutcome::DocumentEmpty, 250, budget));
    CHECK_FALSE(retry_decision(HtmlReadOutcome::DocumentEmpty, 400, budget));
    CHECK_FALSE(retry_decision(HtmlReadOutcome::NoAutomation, 0, budget));
    CHECK_FALSE(retry_decision(HtmlReadOutcome::NoRoot, 0, budget));
    CHECK_FALSE(retry_decision(HtmlReadOutcome::NoDocument, 0, budget));
    CHECK_FALSE(retry_decision(HtmlReadOutcome::Text, 0, budget));
    CHECK_FALSE(retry_decision(HtmlReadOutcome::DocumentEmpty, 0, HtmlReadBudget{0, 0}));
}

TEST_CASE("HTML viewer read returns empty fast for handle 0 or an invalid window",
          "[html_viewer][perf]") {
    const auto start = std::chrono::steady_clock::now();
    CHECK(read_html_viewer_text(0).text.empty());
    CHECK(read_html_viewer_text(0x7ffffff0).text.empty());
    CHECK(read_html_viewer_text(0x7ffffff0, HtmlReadBudget{0, 0}).text.empty());
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    CHECK(ms < 20);
}

TEST_CASE("handle_hosts_browser is false for a STATIC window", "[html_viewer]") {
    CHECK_FALSE(handle_hosts_browser(0));
    HWND wnd = CreateWindowExW(0, L"STATIC", L"x", WS_POPUP, 0, 0, 10, 10, nullptr, nullptr,
                               GetModuleHandleW(nullptr), nullptr);
    REQUIRE(wnd != nullptr);
    CHECK_FALSE(handle_hosts_browser(static_cast<int>(reinterpret_cast<uintptr_t>(wnd))));
    DestroyWindow(wnd);
}
