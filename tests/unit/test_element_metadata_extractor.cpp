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
        json result = ElementMetadataExtractor().extract(nullptr);
        REQUIRE(result.is_null());
    }
}



TEST_CASE("ElementMetadataExtractor - options and cache belong to one extractor (one screen read)", "[metadata][extractor]") {
    // review 2026-10-25: the cache and skip_trees were process-wide statics every reader had to reset by hand
    ElementMetadataExtractor::Options skip;
    skip.skip_trees = true;
    const ElementMetadataExtractor a(skip);
    const ElementMetadataExtractor b;
    CHECK(a.options().skip_trees);
    CHECK_FALSE(b.options().skip_trees);  // a's options do not leak into b
    CHECK(a.cached_entries() == 0);
    CHECK(b.cached_entries() == 0);
}
