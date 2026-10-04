#include <catch2/catch_test_macros.hpp>
#include "include/element_errors.h"

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
