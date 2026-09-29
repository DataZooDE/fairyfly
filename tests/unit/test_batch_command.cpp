#include <catch2/catch_test_macros.hpp>

#include "include/commands/batch_command.h"
#include "include/read_only_guard.h"

using fairyfly::commands::parse_batch_line;
using fairyfly::sap::is_state_changing_action;
using fairyfly::sap::is_state_changing_vkey;
using fairyfly::sap::make_read_only_refusal;
using fairyfly::sap::normalize_menu_segment;
using fairyfly::sap::read_only_doubleclick_rule;
using fairyfly::sap::read_only_vkey_rule;

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

TEST_CASE("parse_batch_line rejects embedded NUL characters", "[batch]") {
    const auto json_line = parse_batch_line(R"(["fill","wnd[0]/usr/txtA","abc\u0000def"])");
    CHECK_FALSE(json_line.ok);
    CHECK(json_line.argv.empty());
    CHECK(json_line.error.find("NUL") != std::string::npos);

    const auto shell_line = parse_batch_line(std::string("tcode SM\0" "37", 11));
    CHECK_FALSE(shell_line.ok);
    CHECK(shell_line.error.find("NUL") != std::string::npos);

    CHECK(parse_batch_line(R"(["tcode","SM37"])").ok);
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

TEST_CASE("read-only send-key uses an allowlist", "[readonly]") {
    for (int allowed : {1, 3, 4, 7, 8, 12, 15, 80, 81, 82, 83}) {
        INFO("vkey " << allowed);
        CHECK(read_only_vkey_rule(allowed, 0).empty());
        CHECK(read_only_vkey_rule(allowed, 1).empty());
    }
    // f2, f5, f6, f9, f10, f11, shift+f2 (14), other shift+f keys and ctrl combos (raw ints).
    for (int refused : {2, 5, 6, 9, 10, 11, 13, 14, 16, 17, 18, 19, 20, 21, 22, 23, 24,
                        25, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42,
                        43, 44, 45, 46, 47, 48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 58, 59, 60,
                        61, 62, 63, 64, 65, 66, 67, 68, 69, 70, 71, 72, 73, 74, 75, 76, 77, 78, 79,
                        84, 99}) {
        INFO("vkey " << refused);
        CHECK(read_only_vkey_rule(refused, 0) == "vkey:" + std::to_string(refused));
        CHECK(is_state_changing_vkey(refused));
    }
    CHECK(read_only_vkey_rule(11, 0) == "vkey:11");
    CHECK(read_only_vkey_rule(14, 0) == "vkey:14");
}

TEST_CASE("read-only Enter is allowed on the main window only", "[readonly]") {
    CHECK(read_only_vkey_rule(0, 0).empty());
    CHECK(read_only_vkey_rule(0, 1) == "vkey:0");
    CHECK(read_only_vkey_rule(0, 2) == "vkey:0");
    CHECK_FALSE(is_state_changing_vkey(0));
}

TEST_CASE("read-only double-click inspects the addressed cell or node", "[readonly]") {
    CHECK(read_only_doubleclick_rule("GuiShell", "", "", "wnd[0]/usr/cntl/shellcont/shell", "Dump 2024").empty());
    CHECK(read_only_doubleclick_rule("GuiShell", "", "", "wnd[0]/usr/shell", "Release").rfind("word:release", 0) == 0);
    CHECK(read_only_doubleclick_rule("GuiShell", "", "", "wnd[0]/usr/shell", "Delete entry") == "word:delete");
    CHECK(read_only_doubleclick_rule("GuiShell", "Save", "", "wnd[0]/usr/shell", "ok") == "word:save");
    CHECK(read_only_doubleclick_rule("GuiShell", "", "", "wnd[0]/usr/shell&SAVE", "ok") == "id:&save");
    CHECK(read_only_doubleclick_rule("GuiShell", "", "", "wnd[0]/usr/shell", "Job log").empty());
}

TEST_CASE("read-only menu paths are normalized before matching", "[readonly]") {
    CHECK(normalize_menu_segment("Sa&ve") == "save");
    CHECK(normalize_menu_segment("  SAVE ") == "save");
    CHECK(denied(normalize_menu_segment("Sa&ve").c_str(), "", "", "GuiMenu"));
    CHECK(denied(normalize_menu_segment("D&ELETE").c_str(), "", "", "GuiMenu"));
    CHECK_FALSE(denied(normalize_menu_segment("&Display").c_str(), "", "", "GuiMenu"));
    // The resolved item text is what really gets selected: judged with the same rules.
    CHECK(denied(normalize_menu_segment("&Release").c_str(), "", "", "GuiMenu"));
}

TEST_CASE("read-only refusal never echoes the text of input elements", "[readonly]") {
    for (const char* type : {"GuiPasswordField", "GuiTextField", "GuiCTextField", "GuiComboBox"}) {
        const auto refusal = make_read_only_refusal("wnd[0]/usr/pwdRSYST-BCODE", type, "secret", "tip",
                                                    "fill:disabled-in-read-only");
        CHECK(refusal.to_json().dump().find("secret") == std::string::npos);
        CHECK(refusal.error["element_type"] == type);
        CHECK(refusal.error["rule"] == "fill:disabled-in-read-only");
    }
    const auto button = make_read_only_refusal("wnd[0]/tbar[0]/btn[11]", "GuiButton", "Save", "Save (Ctrl+S)",
                                               "id:tbar[0]/btn[11]");
    CHECK(button.error["text"] == "Save");
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
