// UTF-8 <-> UTF-16 in one place (text/utf8.h): a strict pair that throws on invalid input (strings crossing into
// COM), and a lossy pair that never throws (logging, Win32 names, credentials: invalid input becomes U+FFFD).
#include <catch2/catch_test_macros.hpp>

#include "include/text/utf8.h"

#include <stdexcept>
#include <string>

using namespace fairyfly::text;

namespace {
// "Ä€😀": two-, three- and four-byte UTF-8 sequences (the last one is a surrogate pair in UTF-16)
const std::string kUtf8 = "\xC3\x84\xE2\x82\xAC\xF0\x9F\x98\x80";
const std::wstring kWide = L"Ä€\U0001F600";
const std::string kInvalidUtf8 = "a\xFF" "b";
const std::wstring kLoneSurrogate = std::wstring(L"a") + wchar_t(0xD800) + L"b";
}  // namespace

TEST_CASE("utf8: strict conversion round-trips and rejects invalid input", "[text][utf8]") {
    CHECK(utf8_to_wide(kUtf8) == kWide);
    CHECK(wide_to_utf8(kWide) == kUtf8);
    CHECK(utf8_to_wide("").empty());
    CHECK(wide_to_utf8(L"").empty());
    CHECK_THROWS_AS(utf8_to_wide(kInvalidUtf8), std::invalid_argument);
    CHECK_THROWS_AS(wide_to_utf8(kLoneSurrogate), std::invalid_argument);
}

TEST_CASE("utf8: lossy conversion never throws and replaces what is invalid", "[text][utf8]") {
    CHECK(utf8_to_wide_lossy(kUtf8) == kWide);
    CHECK(wide_to_utf8_lossy(kWide) == kUtf8);
    CHECK(utf8_to_wide_lossy(kInvalidUtf8) == L"a�b");
    CHECK(wide_to_utf8_lossy(kLoneSurrogate) == "a\xEF\xBF\xBD" "b");
    CHECK(utf8_to_wide_lossy("").empty());
    CHECK(wide_to_utf8_lossy(std::wstring_view()).empty());
}

TEST_CASE("utf8: a C wide string converts lossily, nullptr as empty (log arguments)", "[text][utf8]") {
    // review 2026-10-25: names were logged through _bstr_t, i.e. in the ANSI code page, not UTF-8
    const wchar_t* none = nullptr;
    CHECK(wide_to_utf8_lossy(none).empty());
    CHECK(wide_to_utf8_lossy(L"TextÄ") == "Text\xC3\x84");
}

TEST_CASE("utf8: an embedded NUL is kept, not taken as the end", "[text][utf8]") {
    const std::string with_nul("a\0b", 3);
    CHECK(utf8_to_wide_lossy(with_nul) == std::wstring(L"a\0b", 3));
    CHECK(wide_to_utf8(std::wstring(L"a\0b", 3)) == with_nul);
}
