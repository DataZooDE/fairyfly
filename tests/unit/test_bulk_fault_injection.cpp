#include <catch2/catch_test_macros.hpp>
#include "include/bulk_screen_reader.h"

using namespace fairyfly::sap;

namespace {
ObjectTreeSource real_source(int* calls) {
    return [calls](const std::string& id, const std::vector<std::string>&) -> std::optional<std::string> {
        ++*calls;
        return std::string(R"({"children":[{"properties":{"Id":")") + id + R"(","Type":"GuiMainWindow"}}]})";
    };
}
const std::vector<std::string> kProps{"Id", "Type"};
} // namespace

TEST_CASE("fault injection wraps the tree source only for the diagnostic modes", "[bulk][diag]") {
    int calls = 0;
    SECTION("no mode or unknown mode leaves the real source untouched") {
        for (const char* mode : {"", "none", "typo"}) {
            calls = 0;
            const auto source = wrap_with_injected_fault(real_source(&calls), mode);
            REQUIRE(source("wnd[0]", kProps).has_value());
            REQUIRE(calls == 1);
        }
    }
    SECTION("unsupported answers nothing and never calls SAP") {
        const auto source = wrap_with_injected_fault(real_source(&calls), "unsupported");
        REQUIRE_FALSE(source("wnd[0]", kProps).has_value());
        REQUIRE(calls == 0);
    }
    SECTION("garbage answers text that is not JSON") {
        const auto source = wrap_with_injected_fault(real_source(&calls), "garbage");
        const auto answer = source("wnd[0]", kProps);
        REQUIRE(answer.has_value());
        REQUIRE_FALSE(parse_object_tree(*answer, "wnd[0]").has_value());
    }
    SECTION("wrongroot answers a well-formed tree for another element") {
        const auto source = wrap_with_injected_fault(real_source(&calls), "wrongroot");
        const auto answer = source("wnd[0]", kProps);
        REQUIRE(answer.has_value());
        REQUIRE_FALSE(parse_object_tree(*answer, "wnd[0]").has_value());
    }
    SECTION("fault raises the server fault and exception raises a plain error") {
        REQUIRE_THROWS_AS(wrap_with_injected_fault(real_source(&calls), "fault")("wnd[0]", kProps), ObjectTreeServerFault);
        REQUIRE_THROWS_AS(wrap_with_injected_fault(real_source(&calls), "exception")("wnd[0]", kProps), std::runtime_error);
        REQUIRE(calls == 0);
    }
    SECTION("injection is only honoured with FAIRYFLY_DIAG=1") {
        REQUIRE(injected_fault_mode(nullptr, "fault") == "");
        REQUIRE(injected_fault_mode("0", "fault") == "");
        REQUIRE(injected_fault_mode("1", nullptr) == "");
        REQUIRE(injected_fault_mode("1", "fault") == "fault");
    }
}
