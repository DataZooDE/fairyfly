#include <catch2/catch_test_macros.hpp>
#include "include/element_errors.h"
#include "include/core.h"

using fairyfly::sap::error_code_for_com_message;

// Found by a live bug hunt: `element get` answered COM_ERROR for a missing element while
// `element click` answered ELEMENT_NOT_FOUND for the same id.
TEST_CASE("a COM lookup miss maps to ELEMENT_NOT_FOUND, other COM failures stay COM_ERROR", "[errors]") {
    CHECK(error_code_for_com_message("Element not found: /app/con[0]/ses[0]/wnd[0]/usr/NO_SUCH_FIELD") == "ELEMENT_NOT_FOUND");
    CHECK(error_code_for_com_message("Element not found: ") == "ELEMENT_NOT_FOUND");
    CHECK(error_code_for_com_message("Failed to read property Text: 0x80020009") == "COM_ERROR");
    CHECK(error_code_for_com_message("") == "COM_ERROR");
    CHECK(error_code_for_com_message("The element was not found in the cache") == "COM_ERROR");
}

TEST_CASE("element not found: one error shape for click, fill and F4", "[element][errors]") {
    using fairyfly::sap::set_element_not_found;
    const auto element_not_found = [](const fairyfly::ElementId& target, const std::string& active,
                                      const std::string& command, const std::string& tail = "") {
        fairyfly::Result r;
        set_element_not_found(r, target, active, command, tail);
        return r;
    };
    const fairyfly::ElementId target("/app/con[0]/ses[0]/wnd[0]/usr/txtA");
    // F4: no window analysis
    const fairyfly::Result f4 = element_not_found(target, "", "");
    CHECK(f4.error["code"] == "ELEMENT_NOT_FOUND");
    CHECK(f4.error["message"] == "Element not found at path: /app/con[0]/ses[0]/wnd[0]/usr/txtA");
    CHECK(f4.error["element"] == "/app/con[0]/ses[0]/wnd[0]/usr/txtA");
    CHECK(f4.error["suggestions"].size() == 3);
    CHECK_FALSE(f4.error.contains("window_mismatch"));
    // same window: the three suggestions only
    const fairyfly::Result same = element_not_found(target, "/app/con[0]/ses[0]/wnd[0]", "element click");
    CHECK(same.error["suggestions"].size() == 3);
    CHECK_FALSE(same.error.contains("window_mismatch"));
    // a popup is active: how to retry there
    const fairyfly::Result popup = element_not_found(target, "/app/con[0]/ses[0]/wnd[1]", "element fill", " \"<value>\"");
    CHECK(popup.error["window_mismatch"] == true);
    CHECK(popup.error["active_window"] == "/app/con[0]/ses[0]/wnd[1]");
    CHECK(popup.error["requested_window"] == "/app/con[0]/ses[0]/wnd[0]");
    REQUIRE(popup.error["suggestions"].size() == 6);
    CHECK(popup.error["suggestions"][3] == "Active window is /app/con[0]/ses[0]/wnd[1], not /app/con[0]/ses[0]/wnd[0] - did a popup open?");
    CHECK(popup.error["suggestions"][4] == "Try: fairyfly element fill /app/con[0]/ses[0]/wnd[1]/usr/txtA \"<value>\"");
    CHECK(popup.error["suggestions"][5] == "Or use: fairyfly element fill @active/usr/txtA \"<value>\"");
}
