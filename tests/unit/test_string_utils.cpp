// The shared string helpers (string_utils.h): one lower/upper/trim/iequals instead of a copy per file.
#include <catch2/catch_test_macros.hpp>

#include "include/string_utils.h"
#include "include/com/wrapper.h"

#include <optional>

using namespace fairyfly::utils;

TEST_CASE("string utils: ASCII case folding, independent of the locale", "[utils][string]") {
    CHECK(lower_ascii("GuiTextField ÄÖ") == "guitextfield ÄÖ");
    CHECK(upper_ascii("vbak-auart ß") == "VBAK-AUART ß");
    CHECK(lower_ascii("") == "");
    CHECK(lower_ascii(std::string("\xC4", 1)) == std::string("\xC4", 1));  // a Latin-1 byte stays as it is
}

TEST_CASE("string utils: trim removes ASCII white space at both ends only", "[utils][string]") {
    CHECK(trim("  a b \t\r\n") == "a b");
    CHECK(trim("\f\vx\v") == "x");
    CHECK(trim("   ") == "");
    CHECK(trim("") == "");
    CHECK(trim("x") == "x");
}

TEST_CASE("string utils: trim_view trims without allocating, char helpers are ASCII only", "[utils][string]") {
    const std::string s = "  Host: x \r\n";
    const std::string_view v = trim_view(s);
    CHECK(v == "Host: x");
    CHECK(v.data() == s.data() + 2);  // a view into the input
    CHECK(trim_view("   ").empty());
    CHECK(to_lower_ascii('Q') == 'q');
    CHECK(to_lower_ascii('\xC4') == '\xC4');
    CHECK(is_ascii_alnum('z'));
    CHECK(is_ascii_alnum('7'));
    CHECK_FALSE(is_ascii_alnum('_'));
    CHECK_FALSE(is_ascii_alnum('\xE4'));
}

TEST_CASE("string utils: iequals compares ASCII case-insensitively", "[utils][string]") {
    CHECK(iequals("LocalHost", "localhost"));
    CHECK_FALSE(iequals("localhost", "localhost."));
    CHECK(iequals("", ""));
    CHECK_FALSE(iequals("ä", "Ä"));  // not ASCII: compared as bytes
}

TEST_CASE("string utils: slugify keeps its rules", "[utils][string]") {
    CHECK(slugify("Change Sales Order: Initial Screen") == "change_sales_order_initial_screen");
    CHECK(slugify("  --a--b--  ") == "a_b");
    CHECK(slugify("") == "screenshot");  // the default name
    CHECK(slugify("§$%") == "screenshot");
}

TEST_CASE("string utils: screenshot names are slug_timestamp_random.png", "[utils][string]") {
    const std::string name = generate_screenshot_filename("VA02 Display");
    CHECK(name.rfind("va02_display_", 0) == 0);
    CHECK(name.size() == std::string("va02_display_").size() + 14 + 1 + 4 + 4);
    CHECK(name.substr(name.size() - 4) == ".png");
}

TEST_CASE("trim with a character set: only those characters, at both ends", "[strings]") {
    CHECK(fairyfly::utils::trim(" \t a b \r\n", " \t\r\n") == "a b");
    CHECK(fairyfly::utils::trim("\vtrue", " \t\r\n") == "\vtrue");   // a vertical tab is not in the set
    CHECK(fairyfly::utils::trim(" \t ", " \t\r\n").empty());
}

TEST_CASE("a check value with a vertical tab is not a check value, as before the shared trim", "[strings][com]") {
    // final review 2026-10-27: the checkbox/combo input trims space, tab, CR and LF only
    CHECK_FALSE(fairyfly::sap::ComGuiElement::parse_check_value("\vtrue").has_value());
    CHECK(fairyfly::sap::ComGuiElement::parse_check_value(" true\r\n") == std::optional<bool>(true));
}

TEST_CASE("string utils: SAP's no-break and zero-width spaces become plain", "[utils][string][console]") {
    CHECK(normalize_spaces("Work\xC2\xA0process") == "Work process");
    CHECK(normalize_spaces("a\xE2\x80\xAF" "b\xE2\x80\x87" "c") == "a b c");
    CHECK(normalize_spaces("\xEF\xBB\xBFli\xE2\x80\x8Bne") == "line");
    CHECK(normalize_spaces("Gr\xC3\xBC\xC3\x9F" "e \xE2\x82\xAC") == "Gr\xC3\xBC\xC3\x9F" "e \xE2\x82\xAC");  // other UTF-8 kept
    CHECK(normalize_spaces(std::string("x\xC2", 2)) == std::string("x\xC2", 2));                    // a cut sequence kept
    CHECK(normalize_spaces("") == "");
}
