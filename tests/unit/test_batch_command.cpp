#include <catch2/catch_test_macros.hpp>

#include "include/commands/batch_command.h"

using fairyfly::commands::parse_batch_line;

namespace {
using Args = std::vector<std::string>;
}

TEST_CASE("parse_batch_line skips blanks and comments", "[batch]") {
    CHECK(parse_batch_line("").skip);
    CHECK(parse_batch_line("   \t").skip);
    CHECK(parse_batch_line("# a comment").skip);
    CHECK(parse_batch_line("  # indented comment").skip);
    CHECK(parse_batch_line("[]").skip);
    CHECK(parse_batch_line("\xEF\xBB\xBF# bom comment").skip);
}

TEST_CASE("parse_batch_line handles shell-style words", "[batch]") {
    auto p = parse_batch_line("transaction start SM37");
    REQUIRE(p.ok);
    CHECK(p.argv == Args{"transaction", "start", "SM37"});

    p = parse_batch_line("  element  fill  wnd[0]/usr/txtX   \"hello world\"  ");
    REQUIRE(p.ok);
    CHECK(p.argv == Args{"element", "fill", "wnd[0]/usr/txtX", "hello world"});

    p = parse_batch_line("element fill a b\\ c");
    REQUIRE(p.ok);
    CHECK(p.argv == Args{"element", "fill", "a", "b c"});

    p = parse_batch_line("element fill x \"say \\\"hi\\\"\"");
    REQUIRE(p.ok);
    CHECK(p.argv == Args{"element", "fill", "x", "say \"hi\""});

    p = parse_batch_line("element fill x \"\"");
    REQUIRE(p.ok);
    CHECK(p.argv == Args{"element", "fill", "x", ""});

    p = parse_batch_line("batch --file C:\\tmp\\cmds.txt");
    REQUIRE(p.ok);
    CHECK(p.argv == Args{"batch", "--file", "C:\\tmp\\cmds.txt"});
}

TEST_CASE("parse_batch_line handles JSON arrays", "[batch]") {
    auto p = parse_batch_line("[\"transaction\",\"start\",\"SM37\"]");
    REQUIRE(p.ok);
    CHECK(p.argv == Args{"transaction", "start", "SM37"});

    p = parse_batch_line("[\"element\", \"fill\", \"a b\", \"\"]");
    REQUIRE(p.ok);
    CHECK(p.argv == Args{"element", "fill", "a b", ""});

    CHECK_FALSE(parse_batch_line("[\"transaction\", 5]").ok);
    CHECK_FALSE(parse_batch_line("[\"transaction\"").ok);
}

TEST_CASE("parse_batch_line rejects embedded NUL characters", "[batch]") {
    const auto json_line = parse_batch_line(R"(["element","fill","wnd[0]/usr/txtA","abc\u0000def"])");
    CHECK_FALSE(json_line.ok);
    CHECK(json_line.argv.empty());
    CHECK(json_line.error.find("NUL") != std::string::npos);

    const auto shell_line = parse_batch_line(std::string("tcode SM\0" "37", 11));
    CHECK_FALSE(shell_line.ok);
    CHECK(shell_line.error.find("NUL") != std::string::npos);

    CHECK(parse_batch_line(R"(["transaction","start","SM37"])").ok);
}

TEST_CASE("parse_batch_line rejects unterminated quotes", "[batch]") {
    const auto p = parse_batch_line("element fill x \"oops");
    CHECK_FALSE(p.ok);
    CHECK_FALSE(p.error.empty());
}

