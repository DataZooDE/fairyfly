#include <catch2/catch_test_macros.hpp>
#include <spdlog/spdlog.h>
#include "include/com_wrapper.h"

using namespace fairyfly;
using namespace fairyfly::sap;

TEST_CASE("ComException creation and properties", "[com][exception]") {
    SECTION("ComException stores message") {
        ComException ex("Test error");
        REQUIRE(std::string(ex.what()) == "Test error");
    }

    SECTION("ComException stores HRESULT") {
        HRESULT hr = 0x80004005;
        ComException ex("Test", hr);
        REQUIRE(ex.hresult() == hr);
    }

    SECTION("ComException can be caught as std::runtime_error") {
        REQUIRE_THROWS_AS(throw ComException("Test"), std::runtime_error);
    }
}

TEST_CASE("ComGuiApplication initialization", "[com][gui_application]") {
    SECTION("ComGuiApplication::create() returns valid pointer or throws") {
        try {
            auto app = ComGuiApplication::create();
            REQUIRE(app != nullptr);
            spdlog::info("SAP GUI found - initialization successful");
            SUCCEED();
        } catch (const ComException& e) {
            spdlog::warn("SAP GUI not available: {}", e.what());
            SUCCEED("ComException thrown as expected when SAP not available");
        }
    }
}

TEST_CASE("ComGuiApplication with SAP GUI present", "[com][gui_application][!mayfail]") {
    ComGuiApplicationPtr app;
    try {
        app = ComGuiApplication::create();
    } catch (const ComException& e) {
        SKIP("SAP GUI not available: " + std::string(e.what()));
    }

    REQUIRE(app != nullptr);

    SECTION("Can get app object") {
        REQUIRE(app->get_app_object() != nullptr);
    }

    SECTION("Can check connection count") {
        int count = app->get_connection_count();
        REQUIRE(count >= 0);
        CAPTURE(count);
    }

    SECTION("has_connections matches get_connection_count") {
        bool has = app->has_connections();
        int count = app->get_connection_count();
        REQUIRE(has == (count > 0));
    }
}

TEST_CASE("ComGuiConnection properties", "[com][gui_connection][!mayfail]") {
    ComGuiApplicationPtr app;
    try {
        app = ComGuiApplication::create();
    } catch (const ComException& e) {
        SKIP("SAP GUI not available");
    }

    int conn_count = app->get_connection_count();
    if (conn_count == 0) {
        SKIP("No SAP connections available");
    }

    auto conn = app->get_connection(0);
    REQUIRE(conn != nullptr);

    SECTION("Connection has valid ID") {
        std::string id = conn->get_id();
        REQUIRE(!id.empty());
        CAPTURE(id);
    }

    SECTION("Connection has description") {
        std::string desc = conn->get_description();
        REQUIRE(!desc.empty());
        CAPTURE(desc);
    }

    SECTION("Session count is valid") {
        int sess_count = conn->get_session_count();
        REQUIRE(sess_count >= 0);
        REQUIRE(sess_count <= 6);  // SAP hard limit
        CAPTURE(sess_count);
    }
}

TEST_CASE("ComGuiSession basic operations", "[com][gui_session][!mayfail]") {
    ComGuiApplicationPtr app;
    try {
        app = ComGuiApplication::create();
    } catch (const ComException& e) {
        SKIP("SAP GUI not available");
    }

    if (app->get_connection_count() == 0) {
        SKIP("No SAP connections");
    }

    auto conn = app->get_connection(0);
    if (conn->get_session_count() == 0) {
        SKIP("No active sessions");
    }

    auto session = conn->get_session(0);
    REQUIRE(session != nullptr);

    SECTION("Session has valid ID") {
        std::string id = session->get_id();
        REQUIRE(!id.empty());
        CAPTURE(id);
    }

    SECTION("Can check if session is alive") {
        bool alive = session->is_alive();
        REQUIRE(alive);
    }

    SECTION("Can check busy status") {
        bool busy = session->is_busy();
        REQUIRE(std::is_same_v<decltype(busy), bool>);
        CAPTURE(busy);
    }

    SECTION("Can get active window") {
        auto window = session->get_active_window();
        REQUIRE(window != nullptr);
    }
}

TEST_CASE("ComGuiWindow operations", "[com][gui_window][!mayfail]") {
    ComGuiApplicationPtr app;
    try {
        app = ComGuiApplication::create();
    } catch (const ComException& e) {
        SKIP("SAP GUI not available");
    }

    if (app->get_connection_count() == 0) {
        SKIP("No SAP connections");
    }

    auto conn = app->get_connection(0);
    if (conn->get_session_count() == 0) {
        SKIP("No active sessions");
    }

    auto session = conn->get_session(0);
    auto window = session->get_active_window();
    REQUIRE(window != nullptr);

    SECTION("Window has ID") {
        std::string id = window->get_id();
        REQUIRE(!id.empty());
        CAPTURE(id);
    }

    SECTION("Window may have title") {
        std::string title = window->get_title();
        // Title might be empty, but operation should not crash
        CAPTURE(title);
        SUCCEED();
    }

    SECTION("Window child count is non-negative") {
        int count = window->get_child_count();
        REQUIRE(count >= 0);
        CAPTURE(count);
    }

    SECTION("Can traverse children") {
        int count = window->get_child_count();
        if (count > 0) {
            auto child = window->get_child(0);
            REQUIRE(child != nullptr);
        }
    }
}

TEST_CASE("ComGuiElement properties", "[com][gui_element][!mayfail]") {
    ComGuiApplicationPtr app;
    try {
        app = ComGuiApplication::create();
    } catch (const ComException& e) {
        SKIP("SAP GUI not available");
    }

    if (app->get_connection_count() == 0) {
        SKIP("No SAP connections");
    }

    auto conn = app->get_connection(0);
    if (conn->get_session_count() == 0) {
        SKIP("No active sessions");
    }

    auto session = conn->get_session(0);
    auto window = session->get_active_window();

    int child_count = window->get_child_count();
    if (child_count == 0) {
        SKIP("No child elements");
    }

    auto elem = window->get_child(0);
    REQUIRE(elem != nullptr);

    SECTION("Element has ID") {
        std::string id = elem->get_id();
        REQUIRE(!id.empty());
        CAPTURE(id);
    }

    SECTION("Element has type") {
        std::string type = elem->get_type();
        REQUIRE(!type.empty());
        CAPTURE(type);
    }

    SECTION("Element text can be retrieved") {
        std::string text = elem->get_text();
        // Text might be empty, but operation should not crash
        CAPTURE(text);
        SUCCEED();
    }

    SECTION("Element visibility can be checked") {
        bool visible = elem->is_visible();
        CAPTURE(visible);
        SUCCEED();
    }

    SECTION("Element enabled status can be checked") {
        bool enabled = elem->is_enabled();
        CAPTURE(enabled);
        SUCCEED();
    }
}

TEST_CASE("ComGuiElement find_element_by_id", "[com][gui_element][!mayfail]") {
    ComGuiApplicationPtr app;
    try {
        app = ComGuiApplication::create();
    } catch (const ComException& e) {
        SKIP("SAP GUI not available");
    }

    if (app->get_connection_count() == 0) {
        SKIP("No SAP connections");
    }

    auto conn = app->get_connection(0);
    if (conn->get_session_count() == 0) {
        SKIP("No active sessions");
    }

    auto session = conn->get_session(0);

    SECTION("Can find element by standard window ID") {
        try {
            auto elem = session->find_element_by_id("wnd[0]");
            REQUIRE(elem != nullptr);
        } catch (const ComException& e) {
            SKIP("Element not found (expected in some SAP screens)");
        }
    }

    SECTION("Non-existent element throws") {
        REQUIRE_THROWS_AS(session->find_element_by_id("wnd[999]/usr/invalid"),
                         ComException);
    }
}

TEST_CASE("Memory safety of COM wrappers", "[com][memory]") {
    SECTION("Multiple wrapper creations don't cause issues") {
        try {
            auto app1 = ComGuiApplication::create();
            {
                auto app2 = ComGuiApplication::create();
                REQUIRE(app2 != nullptr);
            }
            // app2 destructed, app1 should still be valid
            REQUIRE(app1 != nullptr);
            SUCCEED();
        } catch (const ComException& e) {
            SKIP("SAP GUI not available");
        }
    }

    SECTION("Null shared_ptr handling") {
        ComGuiElementPtr null_elem = nullptr;
        REQUIRE(null_elem == nullptr);
    }
}

TEST_CASE("COM wrapper exception safety", "[com][exceptions]") {
    SECTION("Invalid COM operations throw appropriate exceptions") {
        try {
            // This should raise an error - trying to create with null
            ComGuiElement::create(nullptr);
            FAIL("Should have thrown ComException");
        } catch (const ComException& e) {
            SUCCEED("ComException thrown as expected");
        }
    }
}
