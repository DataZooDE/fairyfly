#include <catch2/catch_test_macros.hpp>

#include "include/commands/batch_command.h"
#include "include/read_only_guard.h"

using fairyfly::commands::parse_batch_line;
using fairyfly::sap::is_state_changing_action;
using fairyfly::sap::is_state_changing_vkey;
using fairyfly::sap::make_read_only_refusal;

namespace {
bool denied(const char* text, const char* tooltip = "", const char* id = "",
            const char* type = "GuiButton") {
    return is_state_changing_action(type, text, tooltip, id);
}
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
    auto p = parse_batch_line("tcode SM37");
    REQUIRE(p.ok);
    CHECK(p.argv == Args{"tcode", "SM37"});

    p = parse_batch_line("  fill  wnd[0]/usr/txtX   \"hello world\"  ");
    REQUIRE(p.ok);
    CHECK(p.argv == Args{"fill", "wnd[0]/usr/txtX", "hello world"});

    p = parse_batch_line("fill a b\\ c");
    REQUIRE(p.ok);
    CHECK(p.argv == Args{"fill", "a", "b c"});

    p = parse_batch_line("fill x \"say \\\"hi\\\"\"");
    REQUIRE(p.ok);
    CHECK(p.argv == Args{"fill", "x", "say \"hi\""});

    p = parse_batch_line("fill x \"\"");
    REQUIRE(p.ok);
    CHECK(p.argv == Args{"fill", "x", ""});

    p = parse_batch_line("batch --file C:\\tmp\\cmds.txt");
    REQUIRE(p.ok);
    CHECK(p.argv == Args{"batch", "--file", "C:\\tmp\\cmds.txt"});
}

TEST_CASE("parse_batch_line handles JSON arrays", "[batch]") {
    auto p = parse_batch_line("[\"tcode\",\"SM37\"]");
    REQUIRE(p.ok);
    CHECK(p.argv == Args{"tcode", "SM37"});

    p = parse_batch_line("[\"fill\", \"a b\", \"\"]");
    REQUIRE(p.ok);
    CHECK(p.argv == Args{"fill", "a b", ""});

    CHECK_FALSE(parse_batch_line("[\"tcode\", 5]").ok);
    CHECK_FALSE(parse_batch_line("[\"tcode\"").ok);
}

TEST_CASE("parse_batch_line rejects unterminated quotes", "[batch]") {
    const auto p = parse_batch_line("fill x \"oops");
    CHECK_FALSE(p.ok);
    CHECK_FALSE(p.error.empty());
}

TEST_CASE("read-only guard allows SM37 lookups", "[readonly]") {
    CHECK_FALSE(denied("Job log"));
    CHECK_FALSE(denied("Job details"));
    CHECK_FALSE(denied("Refresh"));
    CHECK_FALSE(denied("", "Spool"));
    CHECK_FALSE(denied("Step"));
    CHECK_FALSE(denied("Find"));
    CHECK_FALSE(denied("Display"));
    CHECK_FALSE(denied("Back (F3)"));
    CHECK_FALSE(denied("Cancel (F12)"));
    CHECK_FALSE(denied("Execute", "Execute (F8)"));
}

TEST_CASE("read-only guard refuses SM37 state changes", "[readonly]") {
    CHECK(denied("Release"));
    CHECK(denied("Stop active job"));
    CHECK(denied("", "Delete job from database"));
    CHECK(denied("Cancel active job"));
    CHECK(denied("Execute in background"));
}

TEST_CASE("read-only guard on ST22 and SU01", "[readonly]") {
    CHECK_FALSE(denied("Details"));
    CHECK(denied("", "Delete"));
    CHECK_FALSE(denied("Display"));
    CHECK(denied("Create User"));
    CHECK(denied("Change Password"));
    CHECK(denied("Change"));
    CHECK(denied("Unlock"));
    CHECK(denied("Lock"));
    CHECK(denied("Activate"));
    CHECK(denied("Post"));
    CHECK_FALSE(denied("Change Layout..."));
}

TEST_CASE("read-only guard matches whole words and ignores field content", "[readonly]") {
    CHECK_FALSE(denied("Posting date"));
    CHECK_FALSE(denied("Stopwatch"));
    CHECK(denied("&Save"));
    CHECK_FALSE(denied("Delete me", "", "wnd[0]/usr/txtFIELD", "GuiTextField"));
    CHECK(denied("Delete me", "", "wnd[0]/usr/btnX", "GuiMenu"));
}

TEST_CASE("read-only guard matches element ids", "[readonly]") {
    CHECK(denied("", "", "wnd[0]/usr/cntl/shellcont/shell/btn_&DELETE"));
    CHECK(denied("", "", "wnd[0]/usr/shell/btn_&SAVE"));
    CHECK(denied("", "", "wnd[0]/usr/shell/btn_&RELEASE"));
    CHECK(denied("", "", "wnd[0]/tbar[0]/btn[11]"));
    CHECK_FALSE(denied("", "", "wnd[0]/tbar[0]/btn[3]"));
    CHECK_FALSE(denied("", "", "wnd[0]/tbar[0]/btn[110]"));
    CHECK_FALSE(denied("", "", "wnd[0]/tbar[0]/btn[12]"));
}

TEST_CASE("read-only vkeys and refusal shape", "[readonly]") {
    CHECK(is_state_changing_vkey(11));
    CHECK(is_state_changing_vkey(14));
    CHECK_FALSE(is_state_changing_vkey(0));
    CHECK_FALSE(is_state_changing_vkey(12));

    const auto result = make_read_only_refusal("wnd[0]/tbar[0]/btn[11]", "GuiButton", "Save", "Save (Ctrl+S)", "id:tbar[0]/btn[11]");
    CHECK(result.status == fairyfly::Result::Status::Error);
    CHECK(result.error["code"] == "READ_ONLY_REFUSED");
    CHECK(result.error["element_id"] == "wnd[0]/tbar[0]/btn[11]");
    CHECK(result.error["rule"] == "id:tbar[0]/btn[11]");
}
