#include <catch2/catch_test_macros.hpp>
#include "include/element_metadata_extractor.h"
#include "include/com/wrapper.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace fairyfly;
using namespace fairyfly::sap;

// Real extraction is covered with FakeNode in test_com_wrapper.cpp ("[metadata][golden]").
TEST_CASE("ElementMetadataExtractor - Extract with null element", "[metadata][extractor]") {
    SECTION("Returns null for null element") {
        json result = ElementMetadataExtractor::extract(nullptr);
        REQUIRE(result.is_null());
    }
}


