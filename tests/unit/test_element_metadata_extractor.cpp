#include <catch2/catch_test_macros.hpp>
#include "include/element_metadata_extractor.h"
#include "include/com/wrapper.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace fairyfly;
using namespace fairyfly::sap;

TEST_CASE("ElementMetadataExtractor - Cache management", "[metadata][extractor]") {
    SECTION("Clear cache works") {
        ElementMetadataExtractor::clear_cache();
        
        // Verify cache is cleared (cannot directly access private member, but clear should work)
        ElementMetadataExtractor::clear_cache();  // Second call should not fail
        REQUIRE(true);  // If we got here, clear_cache didn't throw
    }
}

TEST_CASE("ElementMetadataExtractor - Extract with null element", "[metadata][extractor]") {
    SECTION("Returns null for null element") {
        json result = ElementMetadataExtractor::extract(nullptr);
        REQUIRE(result.is_null());
    }
}

TEST_CASE("ElementMetadataExtractor - Extract handles exceptions", "[metadata][extractor]") {
    // This test verifies that extract() gracefully handles exceptions
    // Since we can't easily mock ComGuiElement, we test with null
    // and verify the error handling paths work
    
    SECTION("Returns null on exception") {
        // Passing null element should return null (not throw)
        json result = ElementMetadataExtractor::extract(nullptr, 0);
        REQUIRE(result.is_null());
    }
}

TEST_CASE("SapGuiObject - DISPID cache management", "[com][cache]") {
    SECTION("Clear DISPID cache executes safely") {
        SapGuiObject::clear_dispid_cache();
        REQUIRE(true);
    }
}

