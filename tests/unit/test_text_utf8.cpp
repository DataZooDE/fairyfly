// UTF-8 <-> UTF-16 in one place (text/utf8.h): a strict pair that throws on invalid input (strings crossing into
// COM), and a lossy pair that never throws (logging, Win32 names, credentials: invalid input becomes U+FFFD).
#include <catch2/catch_test_macros.hpp>

#include "include/text/utf8.h"

#include <stdexcept>
#include <string>
#include <vector>

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

// ---- the console's output code page (console_output.h) ------------------------------------------------------------
#include "include/console_output.h"

namespace {
// The console's code page, scripted: what CodePageGuard reads and sets.
struct FakeConsole {
    bool attached = true;
    unsigned cp = 437;
    std::vector<unsigned> sets;
    fairyfly::console::CodePageApi api() {
        return {[this] { return cp; },
                [this](unsigned value) { sets.push_back(value); cp = value; return true; },
                [this] { return attached; }};
    }
};
}  // namespace

TEST_CASE("console output: a console on code page 437/850/1252 is switched to UTF-8, nothing else", "[text][console]") {
    using fairyfly::console::needs_utf8_output;
    CHECK(needs_utf8_output(true, 437));
    CHECK(needs_utf8_output(true, 850));
    CHECK(needs_utf8_output(true, 1252));
    CHECK_FALSE(needs_utf8_output(true, 65001));   // already UTF-8
    CHECK_FALSE(needs_utf8_output(false, 437));    // a pipe or a file: bytes are passed on as they are
    // without a console (the test runner's pipe) both calls are harmless and can be repeated
    fairyfly::console::use_utf8_output();
    fairyfly::console::restore_output_code_page();
    fairyfly::console::restore_output_code_page();
}

TEST_CASE("console output: the code page is put back once, at the end or on Ctrl+C", "[text][console]") {
    FakeConsole console;
    fairyfly::console::CodePageGuard guard(console.api());
    CHECK(guard.switch_to_utf8());
    CHECK(console.cp == 65001);
    CHECK(guard.restore());
    CHECK(console.cp == 437);
    CHECK_FALSE(guard.restore());  // the end of the run after a Ctrl+C restore: nothing more
    CHECK(console.sets == std::vector<unsigned>{65001, 437});
}

TEST_CASE("console output: a code page someone else set meanwhile is left alone", "[text][console]") {
    FakeConsole console;
    fairyfly::console::CodePageGuard guard(console.api());
    REQUIRE(guard.switch_to_utf8());
    console.cp = 850;  // another program (or another fairyfly run that ended) changed it
    CHECK_FALSE(guard.restore());
    CHECK(console.cp == 850);
}

TEST_CASE("console output: no console, or already UTF-8: nothing is switched or restored", "[text][console]") {
    FakeConsole piped;
    piped.attached = false;
    fairyfly::console::CodePageGuard g1(piped.api());
    CHECK_FALSE(g1.switch_to_utf8());
    CHECK_FALSE(g1.restore());
    CHECK(piped.sets.empty());

    FakeConsole utf8;
    utf8.cp = 65001;
    fairyfly::console::CodePageGuard g2(utf8.api());
    CHECK_FALSE(g2.switch_to_utf8());
    CHECK_FALSE(g2.restore());
    CHECK(utf8.sets.empty());
}
