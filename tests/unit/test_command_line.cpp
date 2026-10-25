// Windows command-line quoting in one header (text/command_line.h), used by the SAP shortcut launcher, the tray
// and the standalone tray launcher. Every case must survive CommandLineToArgvW unchanged.
#include <catch2/catch_test_macros.hpp>

#include "include/text/command_line.h"

#include <windows.h>
#include <shellapi.h>

#include <string>
#include <vector>

using namespace fairyfly::text;

namespace {
std::vector<std::wstring> split(const std::wstring& command_line) {
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(command_line.c_str(), &argc);
    std::vector<std::wstring> out;
    for (int i = 0; i < argc; ++i) out.emplace_back(argv[i]);
    LocalFree(argv);
    return out;
}

const std::vector<std::wstring> kCases = {
    L"plain", L"with space", L"", L"tab\there", L"say \"hi\"", LR"(trailing\)", LR"(two\\)", LR"(C:\Program Files\SAP\)",
    LR"(back\"quote)", LR"(\\server\share)", L"\"", L"\\", L"a b\\", L"-sysname=Big fox"};
}  // namespace

TEST_CASE("command line: every argument round-trips through CommandLineToArgvW", "[text][command_line]") {
    for (const auto& arg : kCases) {
        CAPTURE(std::string(arg.begin(), arg.end()));
        // the program name is parsed by different rules: put a plain one first
        const auto when_needed = split(L"prog.exe " + quote_argument<wchar_t>(arg));
        REQUIRE(when_needed.size() == 2);
        CHECK(when_needed[1] == arg);
        const auto always = split(L"prog.exe " + quote_argument_always<wchar_t>(arg));
        REQUIRE(always.size() == 2);
        CHECK(always[1] == arg);
    }
}

TEST_CASE("command line: quote only when needed, or always", "[text][command_line]") {
    CHECK(quote_argument<char>("plain") == "plain");
    CHECK(quote_argument<char>("") == "\"\"");
    CHECK(quote_argument<char>("a b") == "\"a b\"");
    CHECK(quote_argument_always<char>("plain") == "\"plain\"");
    CHECK(quote_argument<wchar_t>(L"x\"y") == L"\"x\\\"y\"");
}

TEST_CASE("command line: join quotes the program and each argument", "[text][command_line]") {
    const std::string line = join_command_line<char>("C:\\Program Files\\fairyfly\\fairyfly.exe", {"mcp", "--tray", "a b"});
    CHECK(line == "\"C:\\Program Files\\fairyfly\\fairyfly.exe\" mcp --tray \"a b\"");
}
