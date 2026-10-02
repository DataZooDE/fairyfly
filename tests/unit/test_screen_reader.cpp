#include <catch2/catch_test_macros.hpp>
#include "include/screen_reader.h"
#include "include/table_data_extractor.h"
#include "include/com/wrapper.h"
#include "include/sensitive_data.h"
#include "include/element_renderers.h"
#include "include/formatters/screen_markdown_formatter.h"
#include "include/cli_handler.h"
#include <vector>
#include <algorithm>

using namespace fairyfly;
using namespace fairyfly::sap;

// Redaction markers carry a reason: "[REDACTED: <reason>]".
static bool is_redacted(const std::string& value) { return fairyfly::sap::is_redaction_marker(value); }

TEST_CASE("ABAP editor bypasses tree extraction", "[screen][editor]") {
    REQUIRE(classify_shell_extraction("GridView") == ShellExtractionKind::Grid);
    REQUIRE(classify_shell_extraction("AbapEditor") == ShellExtractionKind::Metadata);
    REQUIRE(classify_shell_extraction("TextEdit") == ShellExtractionKind::Metadata);
    REQUIRE(classify_shell_extraction("Tree") == ShellExtractionKind::Tree);
}

TEST_CASE("HTML viewer bypasses tree extraction", "[screen][html]") {
    REQUIRE(classify_shell_extraction("HTMLViewer") == ShellExtractionKind::Metadata);
}

TEST_CASE("Screen Markdown identifies unreadable HTML viewer", "[screen][html][markdown]") {
    renderers::register_all_renderers();
    const nlohmann::json viewer = {
        {"id", "wnd[0]/usr/html"}, {"type", "GuiShell"},
        {"subtype", "HTMLViewer"}, {"content_available", false}
    };
    const nlohmann::json data = {
        {"title", "Database check"}, {"transaction", "SE14"},
        {"elements", nlohmann::json::array({viewer})},
        {"hierarchy", {{"other", nlohmann::json::array({viewer})}}}
    };
    const auto md = fairyfly::cli::ScreenMarkdownFormatter::format(data);
    REQUIRE(md.find("HTML viewer") != std::string::npos);
    REQUIRE(md.find("content unavailable") != std::string::npos);
}

TEST_CASE("ScreenReader - Read with no session", "[screen][reader]") {
    ScreenReader reader(nullptr);

    SECTION("Returns error when no session") {
        Result result = reader.read(true);
        REQUIRE(result.status == Result::Status::Error);
        REQUIRE(result.error["code"] == "NO_SESSION");
    }

    SECTION("Read with tabs returns error when no session") {
        Result result = reader.read_with_tabs();
        REQUIRE(result.status == Result::Status::Error);
        REQUIRE(result.error["code"] == "NO_SESSION");
    }
}

TEST_CASE("Expanded screen read restores the previously selected tabs", "[screen][tabs]") {
    const json tabs = json::array({
        {{"id", "wnd[0]/usr/tabsMAIN"}, {"type", "GuiTabStrip"}},
        {{"id", "wnd[0]/usr/tabsMAIN/tabpSPOOL"}, {"type", "GuiTab"}},
        {{"id", "wnd[0]/usr/tabsMAIN/tabpOUTPUT"}, {"type", "GuiTab"}}
    });
    const auto snapshot = TabSelectionSnapshot::capture(tabs,
        [](const std::string& strip_id) {
            REQUIRE(strip_id == "wnd[0]/usr/tabsMAIN");
            return "wnd[0]/usr/tabsMAIN/tabpSPOOL";
        });
    REQUIRE(snapshot.selected_ids ==
            std::vector<std::string>{"wnd[0]/usr/tabsMAIN/tabpSPOOL"});

    std::string active = "wnd[0]/usr/tabsMAIN/tabpOUTPUT";
    REQUIRE(snapshot.restore([&](const std::string& id) { active = id; }));
    REQUIRE(active == "wnd[0]/usr/tabsMAIN/tabpSPOOL");
}

TEST_CASE("Tab selection restoration runs in reverse strip order", "[screen][tabs]") {
    const json tabs = json::array({
        {{"id", "outer"}, {"type", "GuiTabStrip"}},
        {{"id", "inner"}, {"type", "GuiTabStrip"}}
    });
    const auto snapshot = TabSelectionSnapshot::capture(tabs,
        [](const std::string& id) { return id + "/selected"; });
    std::vector<std::string> restored;
    REQUIRE(snapshot.restore([&](const std::string& id) { restored.push_back(id); }));
    REQUIRE(restored == std::vector<std::string>{"inner/selected", "outer/selected"});
}

TEST_CASE("Targeted screen search validates its query and session", "[screen][find]") {
    ScreenReader reader(nullptr);
    ScreenFindOptions query;
    query.id_contains = "RSYST";
    REQUIRE(reader.find(query).error["code"] == "NO_SESSION");
    query.limit = 0;
    REQUIRE(reader.find(query).error["code"] == "INVALID_FIND_LIMIT");
    query.limit = 1;
    query.id_contains.clear();
    REQUIRE(reader.find(query).error["code"] == "EMPTY_FIND_QUERY");
}

TEST_CASE("Targeted screen search matches ID name and type metadata", "[screen][find]") {
    ScreenFindOptions query;
    query.id_contains = "BNAME";
    query.type = "GuiTextField";
    REQUIRE(screen_candidate_matches(query,
        "wnd[0]/usr/txtRSYST-BNAME", "RSYST-BNAME", "GuiTextField"));
    REQUIRE_FALSE(screen_candidate_matches(query,
        "wnd[0]/usr/pwdRSYST-BCODE", "RSYST-BCODE", "GuiPasswordField"));
    query.name_contains = "user";
    REQUIRE_FALSE(screen_candidate_matches(query,
        "wnd[0]/usr/txtRSYST-BNAME", "RSYST-BNAME", "GuiTextField"));
    REQUIRE(screen_candidate_matches(query,
        "wnd[0]/usr/txtRSYST-BNAME", "User name", "GuiTextField"));
}

TEST_CASE("Targeted screen search Markdown shows every returned match", "[screen][find][markdown]") {
    Result result;
    result.status = Result::Status::Success;
    result.data = {
        {"screen_id", "wnd[0]"}, {"title", "Dictionary: Display Table"},
        {"transaction", "SE11"}, {"scanned_count", 35},
        {"match_limit_reached", true}, {"scan_limit_reached", false},
        {"elements", json::array({
            {{"id", "wnd[0]/titl"}, {"type", "GuiTitlebar"},
             {"name", "titl"}, {"text", "Dictionary: Display Table"}}
        })},
        {"hierarchy", {{"other", json::array({
            {{"id", "wnd[0]/titl"}, {"type", "GuiTitlebar"},
             {"name", "titl"}, {"text", "Dictionary: Display Table"}}
        })}}}
    };
    const auto markdown = cli::format_output(result, cli::OutputFormat::Markdown);
    REQUIRE(markdown.find("wnd\\[0\\]/titl") != std::string::npos);
    REQUIRE(markdown.find("Dictionary: Display Table") != std::string::npos);
}

TEST_CASE("Positioned ST22 long text is not a GuiUserArea table", "[screen][reader][userarea]") {
    std::vector<std::tuple<int, int, std::string>> report = {
        {1, 0, "Date and Time"}, {17, 0, "26.09.2026"},
        {31, 0, "16:49:31"}, {40, 0, "(UTC)"},
        {2, 1, "Runtime error"}, {14, 1, "MESSAGE_TYPE_X"},
        {2, 2, "Error analysis"}, {2, 3, "An exception occurred"}
    };
    for (int row = 4; row < 39; ++row)
        report.emplace_back(2, row, "Further error detail");
    REQUIRE_FALSE(ScreenReader::is_tabular_userarea(report));

    const std::vector<std::tuple<int, int, std::string>> table = {
        {0, 0, "MANDT"}, {12, 0, "BNAME"}, {30, 0, "CLASS"},
        {0, 1, "001"}, {12, 1, "DEVELOPER"}, {30, 1, "SUPER"},
        {0, 2, "001"}, {12, 2, "SAP*"}, {30, 2, "SUPER"}
    };
    REQUIRE(ScreenReader::is_tabular_userarea(table));
}

TEST_CASE("Positioned-label table respects the row limit without losing its count", "[screen][reader][userarea]") {
    nlohmann::json table = {
        {"columns", nlohmann::json::array({"JobName", "Status"})},
        {"rows", nlohmann::json::array({
            nlohmann::json::array({"ONE", "Finished"}),
            nlohmann::json::array({"TWO", "Finished"}),
            nlohmann::json::array({"THREE", "Finished"})
        })},
        {"total_row_count", 3}, {"visible_row_count", 3}
    };
    ScreenReader::limit_userarea_table_rows(table, 1);
    REQUIRE(table["rows"].size() == 1);
    REQUIRE(table["rows"][0][0] == "ONE");
    REQUIRE(table["total_row_count"] == 3);
    REQUIRE(table["visible_row_count"] == 3);
}

TEST_CASE("Tree labels use the populated TEXT item after node text is empty", "[screen][tree]") {
    const std::vector<std::string> columns{"1", "2", "TEXT"};
    int reads = 0;
    const auto read_item = [&](const std::string& name) {
        ++reads;
        return name == "TEXT" ? std::string("User") : std::string();
    };
    REQUIRE(recover_tree_node_text("", columns, read_item) == "User");
    REQUIRE(reads == 1);
    REQUIRE(recover_tree_node_text("Existing", columns, read_item) == "Existing");
    REQUIRE(reads == 1);
}

TEST_CASE("ST22 positioned labels remain visible in Markdown", "[screen][markdown][userarea]") {
    const auto labels = nlohmann::json::array({
        {{"id", "wnd[0]/usr/lbl[1,0]"}, {"type", "GuiLabel"},
         {"text", "Date and Time"}, {"grid_row", 0}, {"grid_col", 1}},
        {{"id", "wnd[0]/usr/lbl[1,1]"}, {"type", "GuiLabel"},
         {"text", "Short Text"}, {"grid_row", 1}, {"grid_col", 1}},
        {{"id", "wnd[0]/usr/lbl[40,1]"}, {"type", "GuiLabel"},
         {"text", "OFFSET_TOO_LARGE"}, {"grid_row", 1}, {"grid_col", 40}},
        {{"id", "wnd[0]/usr/lbl[1,2]"}, {"type", "GuiLabel"},
         {"text", "Runtime Errors"}, {"grid_row", 2}, {"grid_col", 1}},
        {{"id", "wnd[0]/usr/lbl[25,2]"}, {"type", "GuiLabel"},
         {"text", "XX"}, {"grid_row", 2}, {"grid_col", 25}},
        {{"id", "wnd[0]/usr/lbl[40,2]"}, {"type", "GuiLabel"},
         {"text", "STRING_OFFSET_TOO_LARGE"}, {"grid_row", 2}, {"grid_col", 40}}
    });
    const nlohmann::json data = {
        {"title", "Runtime Error Long Text"}, {"transaction", "ST22"},
        {"elements", labels}, {"element_count", 6},
        {"hierarchy", {{"form_fields", labels}}}
    };
    const auto md = fairyfly::cli::ScreenMarkdownFormatter::format(data);
    REQUIRE(md.find("## Report Output") != std::string::npos);
    REQUIRE(md.find("Short Text OFFSET_TOO_LARGE") != std::string::npos);
    REQUIRE(md.find("Runtime Errors XX STRING_OFFSET_TOO_LARGE") != std::string::npos);
}

TEST_CASE("Classic report lines are not invented as status properties", "[screen][markdown][userarea]") {
    const auto labels = nlohmann::json::array({
        {{"id", "wnd[0]/usr/lbl[0,2]"}, {"type", "GuiLabel"},
         {"text", "FFLY REPORT OUTPUT 260927"}, {"grid_row", 2}, {"grid_col", 0}},
        {{"id", "wnd[0]/usr/lbl[0,3]"}, {"type", "GuiLabel"},
         {"text", "Diagnostic status: 17 rows"}, {"grid_row", 3}, {"grid_col", 0}},
        {{"id", "wnd[0]/usr/lbl[0,5]"}, {"type", "GuiLabel"},
         {"text", "Completed without changing application data"},
         {"grid_row", 5}, {"grid_col", 0}}
    });
    const nlohmann::json data = {
        {"title", "Disposable classic list verification"}, {"transaction", "SE38"},
        {"elements", labels}, {"element_count", 3},
        {"hierarchy", {{"form_fields", labels}}}
    };
    const auto md = fairyfly::cli::ScreenMarkdownFormatter::format(data);
    REQUIRE(md.find("## Report Output") != std::string::npos);
    REQUIRE(md.find("Diagnostic status: 17 rows") != std::string::npos);
    REQUIRE(md.find("Completed without changing application data") != std::string::npos);
    REQUIRE(md.find("## Status Information") == std::string::npos);
}

TEST_CASE("A one-column report heading does not become a status property", "[screen][markdown][userarea]") {
    const auto labels = nlohmann::json::array({
        {{"id", "wnd[0]/usr/lbl[0,2]"}, {"type", "GuiLabel"},
         {"text", "Delete drafts:"}, {"grid_row", 2}, {"grid_col", 0}},
        {{"id", "wnd[0]/usr/lbl[0,3]"}, {"type", "GuiLabel"},
         {"text", "Deleted 0 drafts"}, {"grid_row", 3}, {"grid_col", 0}}
    });
    const nlohmann::json data = {
        {"title", "Report log"}, {"elements", labels}, {"element_count", 2},
        {"hierarchy", {{"form_fields", labels}}}
    };
    const auto md = fairyfly::cli::ScreenMarkdownFormatter::format(data);
    REQUIRE(md.find("## Report Output") != std::string::npos);
    REQUIRE(md.find("Delete drafts:") != std::string::npos);
    REQUIRE(md.find("Deleted 0 drafts") != std::string::npos);
    REQUIRE(md.find("## Status Information") == std::string::npos);
}

TEST_CASE("Classic report Markdown preserves step spacing and indentation", "[screen][markdown][userarea]") {
    const auto labels = nlohmann::json::array({
        {{"id", "wnd[0]/usr/lbl[0,2]"}, {"type", "GuiLabel"},
         {"text", "Delete drafts..."}, {"grid_row", 2}, {"grid_col", 0}},
        {{"id", "wnd[0]/usr/lbl[0,3]"}, {"type", "GuiLabel"},
         {"text", "   "}, {"grid_row", 3}, {"grid_col", 0}},
        {{"id", "wnd[0]/usr/lbl[3,3]"}, {"type", "GuiLabel"},
         {"text", "Deleted 0 drafts"}, {"grid_row", 3}, {"grid_col", 3}},
        {{"id", "wnd[0]/usr/lbl[3,4]"}, {"type", "GuiLabel"},
         {"text", "done"}, {"grid_row", 4}, {"grid_col", 3}},
        {{"id", "wnd[0]/usr/lbl[0,5]"}, {"type", "GuiLabel"},
         {"text", " "}, {"grid_row", 5}, {"grid_col", 0}},
        {{"id", "wnd[0]/usr/lbl[0,6]"}, {"type", "GuiLabel"},
         {"text", " "}, {"grid_row", 6}, {"grid_col", 0}},
        {{"id", "wnd[0]/usr/lbl[0,7]"}, {"type", "GuiLabel"},
         {"text", "Generate EPM data..."}, {"grid_row", 7}, {"grid_col", 0}}
    });
    const nlohmann::json data = {
        {"title", "EPM list shape"}, {"elements", labels}, {"element_count", labels.size()},
        {"hierarchy", {{"other", labels}}}
    };
    const auto md = fairyfly::cli::ScreenMarkdownFormatter::format(data);
    REQUIRE(md.find("Delete drafts...\n   Deleted 0 drafts\n   done\n\n\nGenerate EPM data...") !=
            std::string::npos);
}

TEST_CASE("Classic report Markdown preserves full 8-phase EPM generator output structure", "[screen][markdown][userarea]") {
    nlohmann::json::array_t labels;
    int row = 2;
    auto add_label = [&](int r, int c, const std::string& text) {
        labels.push_back({
            {"id", "wnd[0]/usr/lbl[" + std::to_string(c) + "," + std::to_string(r) + "]"},
            {"type", "GuiLabel"},
            {"text", text},
            {"grid_row", r},
            {"grid_col", c}
        });
    };

    // Phase 1: Delete drafts...
    add_label(row++, 0, "Delete drafts...");
    add_label(row++, 3, "Deleted 0 drafts of BO /BOBF/DEMO_CUSTOMER");
    add_label(row++, 3, "Deleted 0 drafts of BO SEPMRA_I_PRODUCTWITHDRAFT");
    add_label(row++, 3, "Deleted 0 drafts of BO SEPMRA_I_PURCHASEORDERWD");
    add_label(row++, 3, "Deleted 0 drafts of BO SEPMRA_I_SALESORDERTP");
    add_label(row++, 3, "done");
    row += 2; // SKIP 2.

    // Phase 2: Delete favourites...
    add_label(row++, 0, "Delete favourites...");
    add_label(row++, 3, "done");
    row += 2; // SKIP 2.

    // Phase 3: Delete shopping carts...
    add_label(row++, 0, "Delete shopping carts...");
    add_label(row++, 3, "Deleted 0 shopping carts");
    add_label(row++, 3, "done");
    row += 2; // SKIP 2.

    // Phase 4: Delete notifications...
    add_label(row++, 0, "Delete notifications...");
    add_label(row++, 3, "Deleted 0 notifications");
    add_label(row++, 3, "done");
    row += 2; // SKIP 2.

    // Phase 5: Generate EPM data...
    add_label(row++, 0, "Generate EPM data...");
    add_label(row++, 3, "EPM data generated successfully");
    add_label(row++, 3, "done");
    row += 2; // SKIP 2.

    // Phase 6: Create notifications for generated data...
    add_label(row++, 0, "Create notifications for generated data...");
    add_label(row++, 3, "Notifications created");
    add_label(row++, 3, "done");
    row += 2; // SKIP 2.

    // Phase 7: Adjust Sales Order data for analytics
    add_label(row++, 0, "Adjust Sales Order data for analytics");
    add_label(row++, 3, "done");
    row += 2; // SKIP 2.

    // Phase 8: Adjust Review Texts
    add_label(row++, 0, "Adjust Review Texts");
    add_label(row++, 3, "done");

    const nlohmann::json data = {
        {"title", "EPM Generator Output"},
        {"elements", labels},
        {"element_count", labels.size()},
        {"hierarchy", {{"other", labels}}}
    };

    const auto md = fairyfly::cli::ScreenMarkdownFormatter::format(data);

    // Verify all 8 phase headers are present
    CHECK(md.find("Delete drafts...") != std::string::npos);
    CHECK(md.find("Delete favourites...") != std::string::npos);
    CHECK(md.find("Delete shopping carts...") != std::string::npos);
    CHECK(md.find("Delete notifications...") != std::string::npos);
    CHECK(md.find("Generate EPM data...") != std::string::npos);
    CHECK(md.find("Create notifications for generated data...") != std::string::npos);
    CHECK(md.find("Adjust Sales Order data for analytics") != std::string::npos);
    CHECK(md.find("Adjust Review Texts") != std::string::npos);

    // Verify indentation of messages and status
    CHECK(md.find("   Deleted 0 drafts of BO /BOBF/DEMO_CUSTOMER") != std::string::npos);
    CHECK(md.find("   EPM data generated successfully") != std::string::npos);
    CHECK(md.find("   done") != std::string::npos);

    // Verify step separation (blank lines between phases)
    CHECK(md.find("done\n\n\nDelete favourites...") != std::string::npos);
    CHECK(md.find("done\n\n\nDelete shopping carts...") != std::string::npos);
}

TEST_CASE("Classic report Markdown clamps column indentation exceeding 80", "[screen][markdown][userarea]") {
    const auto labels = nlohmann::json::array({
        {{"id", "wnd[0]/usr/lbl[0,0]"}, {"type", "GuiLabel"},
         {"text", "Wide Report"}, {"grid_row", 0}, {"grid_col", 0}},
        {{"id", "wnd[0]/usr/lbl[95,1]"}, {"type", "GuiLabel"},
         {"text", "Indented at col 95"}, {"grid_row", 1}, {"grid_col", 95}}
    });
    const nlohmann::json data = {
        {"title", "Wide Report"}, {"elements", labels}, {"element_count", labels.size()},
        {"hierarchy", {{"other", labels}}}
    };
    const auto md = fairyfly::cli::ScreenMarkdownFormatter::format(data);
    const std::string expected_prefix = std::string(80, ' ') + "Indented at col 95";
    CHECK(md.find(expected_prefix) != std::string::npos);
}



TEST_CASE("Positioned status label and value still render as a property", "[screen][markdown][grid]") {
    const auto labels = nlohmann::json::array({
        {{"id", "wnd[0]/usr/lbl[0,0]"}, {"type", "GuiLabel"},
         {"text", "Queue:"}, {"grid_row", 0}, {"grid_col", 0}},
        {{"id", "wnd[0]/usr/lbl[30,0]"}, {"type", "GuiLabel"},
         {"text", "Workers:"}, {"grid_row", 0}, {"grid_col", 30}},
        {{"id", "wnd[0]/usr/lbl[0,1]"}, {"type", "GuiLabel"},
         {"text", "Ready"}, {"grid_row", 1}, {"grid_col", 0}},
        {{"id", "wnd[0]/usr/lbl[30,1]"}, {"type", "GuiLabel"},
         {"text", "4"}, {"grid_row", 1}, {"grid_col", 30}}
    });
    const nlohmann::json data = {
        {"title", "Status screen"}, {"elements", labels}, {"element_count", 4},
        {"hierarchy", {{"form_fields", labels}}}
    };
    const auto md = fairyfly::cli::ScreenMarkdownFormatter::format(data);
    REQUIRE(md.find("## Status Information") != std::string::npos);
    REQUIRE(md.find("**Queue**") != std::string::npos);
    REQUIRE(md.find("Ready") != std::string::npos);
    REQUIRE(md.find("**Workers**") != std::string::npos);
}

TEST_CASE("Positioned report labels do not expose named credentials", "[screen][privacy][userarea]") {
    nlohmann::json elements = nlohmann::json::array({
        {{"id", "wnd[0]/usr/lbl[0,0]"}, {"type", "GuiLabel"},
         {"text", "Password"}, {"grid_row", 0}, {"grid_col", 0}},
        {{"id", "wnd[0]/usr/lbl[30,0]"}, {"type", "GuiLabel"},
         {"text", "private-report-secret"}, {"grid_row", 0}, {"grid_col", 30}},
        {{"id", "wnd[0]/usr/lbl[0,1]"}, {"type", "GuiLabel"},
         {"text", "Runtime Errors"}, {"grid_row", 1}, {"grid_col", 0}},
        {{"id", "wnd[0]/usr/lbl[30,1]"}, {"type", "GuiLabel"},
         {"text", "STRING_OFFSET_TOO_LARGE"}, {"grid_row", 1}, {"grid_col", 30}}
    });
    redact_sensitive_report_labels(elements);
    REQUIRE(is_redacted(elements[0]["text"]));
    REQUIRE(is_redacted(elements[1]["text"]));
    REQUIRE(elements[2]["text"] == "Runtime Errors");
    REQUIRE(elements[3]["text"] == "STRING_OFFSET_TOO_LARGE");
}

TEST_CASE("Gateway response header credentials are redacted", "[screen][privacy]") {
    nlohmann::json table = {
        {"columns", nlohmann::json::array({"NAME", "VALUE"})},
        {"rows", nlohmann::json::array({
            nlohmann::json::array({"content-type", "application/xml"}),
            nlohmann::json::array({"Set-Cookie", "private-session-value"}),
            nlohmann::json::array({"X-CSRF-Token", "private-csrf-value"}),
            nlohmann::json::array({"Authorization", "private-auth-value"})
        })}
    };

    redact_sensitive_header_rows(table);

    REQUIRE(table["rows"][0][1] == "application/xml");
    REQUIRE(is_redacted(table["rows"][1][1]));
    REQUIRE(is_redacted(table["rows"][2][1]));
    REQUIRE(is_redacted(table["rows"][3][1]));
}

TEST_CASE("Inline credential values in grid labels are redacted", "[screen][privacy]") {
    nlohmann::json table = {
        {"columns", nlohmann::json::array({"NAME", "VALUE"})},
        {"rows", nlohmann::json::array({
            nlohmann::json::array({"Authorization: Bearer private-inline", "other-value"}),
            nlohmann::json::array({"Password=private-inline", "other-value"}),
            nlohmann::json::array({"X-Session-Token", "private-value"})
        })}
    };

    redact_sensitive_header_rows(table);

    REQUIRE(is_redacted(table["rows"][0][0]));
    REQUIRE(is_redacted(table["rows"][0][1]));
    REQUIRE(is_redacted(table["rows"][1][0]));
    REQUIRE(is_redacted(table["rows"][1][1]));
    REQUIRE(table["rows"][2][0] == "X-Session-Token");
    REQUIRE(is_redacted(table["rows"][2][1]));
}

TEST_CASE("Credential columns are redacted without hiding ordinary grid data", "[screen][privacy]") {
    nlohmann::json table = {
        {"columns", nlohmann::json::array({"USER", "PASSWORD", "API_KEY", "DESCRIPTION"})},
        {"rows", nlohmann::json::array({
            nlohmann::json::array({"alice", "private-password", "private-key", "Basis operator"})
        })}
    };

    redact_sensitive_header_rows(table);

    REQUIRE(table["rows"][0][0] == "alice");
    REQUIRE(is_redacted(table["rows"][0][1]));
    REQUIRE(is_redacted(table["rows"][0][2]));
    REQUIRE(table["rows"][0][3] == "Basis operator");
    REQUIRE(is_sensitive_data_name("X-CSRF-Token"));
    REQUIRE_FALSE(is_sensitive_data_name("DESCRIPTION"));
}

TEST_CASE("Header row redaction tolerates localized and missing column metadata", "[screen][privacy]") {
    nlohmann::json localized = {
        {"columns", nlohmann::json::array({"Name", "Wert", "Direction"})},
        {"rows", nlohmann::json::array({
            nlohmann::json::array({"Authorization", "Bearer private-token", "response"})
        })}
    };
    redact_sensitive_header_rows(localized);
    REQUIRE(localized["rows"][0][0] == "Authorization");
    REQUIRE(is_redacted(localized["rows"][0][1]));
    REQUIRE(is_redacted(localized["rows"][0][2]));

    nlohmann::json punctuated = {
        {"columns", nlohmann::json::array({"Header-Name", "Header-Value"})},
        {"rows", nlohmann::json::array({
            nlohmann::json::array({"Set-Cookie", "SAP_SESSIONID=private"})
        })}
    };
    redact_sensitive_header_rows(punctuated);
    REQUIRE(is_redacted(punctuated["rows"][0][1]));

    nlohmann::json no_columns = {
        {"rows", nlohmann::json::array({
            nlohmann::json::array({"X-CSRF-Token", "private-token"})
        })}
    };
    redact_sensitive_header_rows(no_columns);
    REQUIRE_FALSE(no_columns.contains("columns"));
    REQUIRE(is_redacted(no_columns["rows"][0][1]));
}

TEST_CASE("Compound credential names are redacted in headers and columns", "[screen][privacy]") {
    nlohmann::json table = {
        {"columns", nlohmann::json::array({"Header Name", "Header Value", "Session Token Hint"})},
        {"rows", nlohmann::json::array({
            nlohmann::json::array({"X-Session-Token", "private-token", "private-hint"}),
            nlohmann::json::array({"Content-Type", "application/json", "private-hint"}),
            nlohmann::json::array({"X-CSRF-Token", "Token", "private-hint"})
        })}
    };
    redact_sensitive_header_rows(table);
    REQUIRE(table["rows"][0][0] == "X-Session-Token");
    REQUIRE(is_redacted(table["rows"][0][1]));
    REQUIRE(is_redacted(table["rows"][0][2]));
    REQUIRE(table["rows"][1][1] == "application/json");
    REQUIRE(is_redacted(table["rows"][1][2]));
    REQUIRE(table["rows"][2][0] == "X-CSRF-Token");
    REQUIRE(is_redacted(table["rows"][2][1]));
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtGENERIC", "My Secret Key"));
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtS_TOKEN", ""));
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtGENERIC", "Kennwort"));
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtGENERIC", "Passwort"));
}

TEST_CASE("Spanish password labels mask neutral text fields", "[screen][privacy]") {
    const std::string password_label = "Contrase\xC3\xB1" "a";
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtP_VALUE", password_label));
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtP_VALUE", "Contrasena"));
    REQUIRE_FALSE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtP_VALUE", "Flight"));
    const std::string response = std::string("{\"") + password_label +
                                 "\":\"synthetic-spanish-marker\",\"name\":\"Flight\"}";
    const auto redacted = redact_sensitive_response_text(response);
    REQUIRE(redacted.find("synthetic-spanish-marker") == std::string::npos);
    REQUIRE(redacted.find("Flight") != std::string::npos);
}

TEST_CASE("French password labels mask neutral text fields", "[screen][privacy]") {
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtP_VALUE", "Mot de passe"));
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtP_VALUE", "Nouveau mot de passe"));
    REQUIRE_FALSE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtP_VALUE", "Flight"));
    const auto redacted = redact_sensitive_response_text(
        R"({"Mot de passe":"synthetic-french-marker","name":"Flight"})");
    REQUIRE(redacted.find("synthetic-french-marker") == std::string::npos);
    REQUIRE(redacted.find("Flight") != std::string::npos);
}

TEST_CASE("Portuguese password labels mask neutral text fields", "[screen][privacy]") {
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtP_VALUE", "Senha"));
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtP_VALUE", "Nova senha"));
    REQUIRE_FALSE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtP_VALUE", "Flight"));
    const auto redacted = redact_sensitive_response_text(
        R"({"Senha":"synthetic-portuguese-marker","name":"Flight"})");
    REQUIRE(redacted.find("synthetic-portuguese-marker") == std::string::npos);
    REQUIRE(redacted.find("Flight") != std::string::npos);
}

TEST_CASE("Dutch password labels mask neutral text fields", "[screen][privacy]") {
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtP_VALUE", "Wachtwoord"));
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtP_VALUE", "Nieuw wachtwoord"));
    REQUIRE_FALSE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtP_VALUE", "Flight"));
    const auto redacted = redact_sensitive_response_text(
        R"({"Wachtwoord":"synthetic-dutch-marker","name":"Flight"})");
    REQUIRE(redacted.find("synthetic-dutch-marker") == std::string::npos);
    REQUIRE(redacted.find("Flight") != std::string::npos);
}

TEST_CASE("Unicode password labels mask neutral text fields", "[screen][privacy]") {
    const std::vector<std::string> labels = {
        "\xE5\xAF\x86\xE7\xA0\x81", // Chinese
        "\xE3\x83\x91\xE3\x82\xB9\xE3\x83\xAF\xE3\x83\xBC\xE3\x83\x89", // Japanese
        "\xD0\x9F\xD0\xB0\xD1\x80\xD0\xBE\xD0\xBB\xD1\x8C", // Russian
        "\xC5\x9E" "ifre", // Turkish
        "Has\xC5\x82" "o" // Polish
    };
    for (size_t index = 0; index < labels.size(); ++index) {
        INFO("locale index " << index);
        REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtP_VALUE", labels[index]));
        const std::string marker = "synthetic-unicode-marker-" + std::to_string(index);
        const auto redacted = redact_sensitive_response_text(
            "{\"" + labels[index] + "\":\"" + marker + "\",\"name\":\"Flight\"}");
        REQUIRE(redacted.find(marker) == std::string::npos);
    }
    REQUIRE_FALSE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtP_VALUE", "Flight"));
}

TEST_CASE("Truncated JSON masks Unicode-escaped password keys", "[screen][privacy]") {
    const auto chinese = redact_sensitive_response_text(
        R"({"\u5bc6\u7801":"synthetic-escaped-chinese-marker",)");
    REQUIRE(chinese.find("synthetic-escaped-chinese-marker") == std::string::npos);
    const auto japanese = redact_sensitive_response_text(
        R"({"\u30d1\u30b9\u30ef\u30fc\u30c9":"synthetic-escaped-japanese-marker",)");
    REQUIRE(japanese.find("synthetic-escaped-japanese-marker") == std::string::npos);
    const auto unquoted = redact_sensitive_response_text(
        R"({\u5bc6\u7801:synthetic-unquoted-marker,)");
    REQUIRE(unquoted.find("synthetic-unquoted-marker") == std::string::npos);
    const auto ordinary = redact_sensitive_response_text(
        R"({"\u0043ontent-Type":"application/json",)");
    REQUIRE(ordinary.find("application/json") != std::string::npos);
}

TEST_CASE("XML numeric Unicode password names are masked", "[screen][privacy]") {
    const auto hex = redact_sensitive_response_text(
        "<Name>&#x5BC6;&#x7801;</Name><Value>synthetic-xml-unicode-hex</Value>");
    REQUIRE(hex.find("synthetic-xml-unicode-hex") == std::string::npos);
    const auto decimal = redact_sensitive_response_text(
        "<Name>&#23494;&#30721;</Name><Value>synthetic-xml-unicode-decimal</Value>");
    REQUIRE(decimal.find("synthetic-xml-unicode-decimal") == std::string::npos);
    const auto ordinary = redact_sensitive_response_text(
        "<Name>Content-Type</Name><Value>application/json</Value>");
    REQUIRE(ordinary.find("application/json") != std::string::npos);
}

TEST_CASE("Response editor masks JSON credentials and HTTP auth headers", "[screen][privacy]") {
    const std::string body = R"({"data":{"id":7,"access_token":"private-token","name":"Flight"},"items":[{"clientSecret":"private-secret"}]})";
    const auto redacted = redact_sensitive_response_text(body);
    REQUIRE(redacted.find("private-token") == std::string::npos);
    REQUIRE(redacted.find("private-secret") == std::string::npos);
    REQUIRE(redacted.find("\"name\": \"Flight\"") != std::string::npos);
    REQUIRE(redacted.find("\"id\": 7") != std::string::npos);

    const std::string headers = "HTTP/1.1 200 OK\r\nAuthorization: Bearer private-auth\r\n"
                                "Set-Cookie: private-cookie\r\nContent-Type: application/json\r\n";
    const auto redacted_headers = redact_sensitive_response_text(headers);
    REQUIRE(redacted_headers.find("private-auth") == std::string::npos);
    REQUIRE(redacted_headers.find("private-cookie") == std::string::npos);
    REQUIRE(redacted_headers.find("Content-Type: application/json") != std::string::npos);
}

TEST_CASE("Response editor suppresses recognizable XML and broken JSON secrets", "[screen][privacy]") {
    const std::string xml = "<entry><Name>Flight</Name><d:AccessToken>private-xml</d:AccessToken></entry>";
    REQUIRE(redact_sensitive_response_text(xml) == "[REDACTED]");
    const std::string xml_attribute = "<entry clientSecret='private-attribute'><Name>Flight</Name></entry>";
    REQUIRE(redact_sensitive_response_text(xml_attribute) == "[REDACTED]");
    const std::string broken_json = R"({"name":"Flight","access_token":"private-broken")";
    REQUIRE(redact_sensitive_response_text(broken_json) == "[REDACTED]");
    const std::string broken_named_json =
        R"({"headers":[{"Name":"Authorization","Value":"private-auth")";
    REQUIRE(redact_sensitive_response_text(broken_named_json) == "[REDACTED]");
    REQUIRE(redact_sensitive_response_text("<entry><Name>Flight</Name></entry>") ==
            "<entry><Name>Flight</Name></entry>");
    REQUIRE(redact_sensitive_response_text(R"({"name":"Flight")") ==
            R"({"name":"Flight")");
}

TEST_CASE("Response editor suppresses XML values in Unicode element tags and attributes", "[screen][privacy]") {
    const std::string xml_ko = "<entry><Name>Flight</Name><비밀번호>SYNTH_XML_SECRET_KO</비밀번호></entry>";
    REQUIRE(redact_sensitive_response_text(xml_ko) == "[REDACTED]");

    const std::string xml_zh = "<entry><Name>Flight</Name><密码>SYNTH_XML_SECRET_ZH</密码></entry>";
    REQUIRE(redact_sensitive_response_text(xml_zh) == "[REDACTED]");

    const std::string xml_ru = "<entry><Name>Flight</Name><Пароль>SYNTH_XML_SECRET_RU</Пароль></entry>";
    REQUIRE(redact_sensitive_response_text(xml_ru) == "[REDACTED]");

    const std::string xml_sv = "<entry><Name>Flight</Name><Lösenord>SYNTH_XML_SECRET_SV</Lösenord></entry>";
    REQUIRE(redact_sensitive_response_text(xml_sv) == "[REDACTED]");

    const std::string xml_de = "<entry><Name>Flight</Name><Passwort>SYNTH_XML_SECRET_DE</Passwort></entry>";
    REQUIRE(redact_sensitive_response_text(xml_de) == "[REDACTED]");

    const std::string xml_ns = "<entry><Name>Flight</Name><d:비밀번호>SYNTH_XML_SECRET_NAMESPACED</d:비밀번호></entry>";
    REQUIRE(redact_sensitive_response_text(xml_ns) == "[REDACTED]");

    const std::string xml_attr_ko = "<entry 비밀번호=\"SYNTH_XML_ATTR_SECRET_KO\"><Name>Flight</Name></entry>";
    REQUIRE(redact_sensitive_response_text(xml_attr_ko) == "[REDACTED]");

    const std::string xml_attr_zh = "<entry 密码=\"SYNTH_XML_ATTR_SECRET_ZH\"><Name>Flight</Name></entry>";
    REQUIRE(redact_sensitive_response_text(xml_attr_zh) == "[REDACTED]");

    const std::string xml_attr_sv = "<entry Lösenord=\"SYNTH_XML_ATTR_SECRET_SV\"><Name>Flight</Name></entry>";
    REQUIRE(redact_sensitive_response_text(xml_attr_sv) == "[REDACTED]");

    const std::string ordinary = "<entry><Name>Content-Type</Name><Value>application/xml</Value></entry>";
    REQUIRE(redact_sensitive_response_text(ordinary) == ordinary);
}

TEST_CASE("Extended multilingual password labels mask neutral input fields", "[screen][privacy]") {
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtGENERIC", "비밀번호"));
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtGENERIC", "암호"));
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtGENERIC", "Lösenord"));
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtGENERIC", "Passord"));
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtGENERIC", "Adgangskode"));
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtGENERIC", "Parola d'ordine"));
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtGENERIC", "Heslo"));
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtGENERIC", "Jelszó"));
    REQUIRE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtGENERIC", "Salasana"));
    REQUIRE(is_sensitive_data_name("Passwort"));
    REQUIRE(is_sensitive_data_name("Kennwort"));
    REQUIRE(!is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtGENERIC", "Description"));
}

TEST_CASE("Response editor suppresses XML values paired with credential names", "[screen][privacy]") {
    const std::string header =
        "<headers><header><Name>Authorization</Name><Value>FF_XML_SECRET_01</Value></header></headers>";
    REQUIRE(redact_sensitive_response_text(header) == "[REDACTED]");

    const std::string field =
        "<fields><field><Key>client_secret</Key><Value>FF_XML_SECRET_02</Value></field></fields>";
    REQUIRE(redact_sensitive_response_text(field) == "[REDACTED]");

    const std::string cdata =
        "<headers><header><Name><![CDATA[Authorization]]></Name><Value>FF_XML_SECRET_03</Value></header></headers>";
    REQUIRE(redact_sensitive_response_text(cdata) == "[REDACTED]");

    const std::string attributes =
        "<headers><header Name=\"Authorization\" Value=\"FF_XML_SECRET_04\"/></headers>";
    REQUIRE(redact_sensitive_response_text(attributes) == "[REDACTED]");

    const std::string unquoted_attributes =
        "<headers><header Name=Authorization Value=FF_XML_SECRET_05/></headers>";
    REQUIRE(redact_sensitive_response_text(unquoted_attributes) == "[REDACTED]");

    const std::string encoded_element =
        "<headers><header><Name>Authoriz&#97;tion</Name><Value>FF_XML_SECRET_06</Value></header></headers>";
    REQUIRE(redact_sensitive_response_text(encoded_element) == "[REDACTED]");

    const std::string padded_entity =
        "<headers><header><Name>Authoriz&#00000000000000000097;tion</Name><Value>FF_XML_SECRET_08</Value></header></headers>";
    REQUIRE(redact_sensitive_response_text(padded_entity) == "[REDACTED]");

    const std::string encoded_attribute =
        "<headers><header Name=\"client&#x5F;secret\" Value=\"FF_XML_SECRET_07\"/></headers>";
    REQUIRE(redact_sensitive_response_text(encoded_attribute) == "[REDACTED]");

    const std::string truncated =
        "<headers><header><Name>Authorization<Value>FF_XML_SECRET_09</Value>";
    REQUIRE(redact_sensitive_response_text(truncated) == "[REDACTED]");

    const std::string bom_prefixed =
        "\xEF\xBB\xBF<headers><header><Name>Authorization</Name><Value>FF_XML_SECRET_10</Value></header></headers>";
    REQUIRE(redact_sensitive_response_text(bom_prefixed) == "[REDACTED]");

    const std::string bom_json = "\xEF\xBB\xBF{\"access_token\":\"FF_XML_SECRET_11\"}";
    REQUIRE(redact_sensitive_response_text(bom_json).find("FF_XML_SECRET_11") == std::string::npos);

    const std::string ordinary =
        "<headers><header><Name>Content-Type</Name><Value>application/json</Value></header></headers>";
    REQUIRE(redact_sensitive_response_text(ordinary) == ordinary);

    const std::string ordinary_attributes =
        "<headers><header Name=Content-Type Value=application/json/></headers>";
    REQUIRE(redact_sensitive_response_text(ordinary_attributes) == ordinary_attributes);

    const std::string ordinary_encoded =
        "<headers><header><Name>Content&#45;Type</Name><Value>application/json</Value></header></headers>";
    REQUIRE(redact_sensitive_response_text(ordinary_encoded) == ordinary_encoded);

    const std::string ordinary_truncated =
        "<headers><header><Name>Content-Type<Value>application/json</Value>";
    REQUIRE(redact_sensitive_response_text(ordinary_truncated) == ordinary_truncated);
}

TEST_CASE("Response editor masks generic values paired with credential names", "[screen][privacy]") {
    const auto result = redact_sensitive_response_text(
        R"({"headers":[{"Name":"Authorization","Value":"private-auth"},{"Name":"Content-Type","Value":"application/json"}],"data":{"value":"ordinary"}})");
    REQUIRE(result.find("private-auth") == std::string::npos);
    REQUIRE(result.find("\"Name\": \"Authorization\"") != std::string::npos);
    REQUIRE(result.find("application/json") != std::string::npos);
    REQUIRE(result.find("ordinary") != std::string::npos);
}

TEST_CASE("Response editor masks credential values in JSON header pairs", "[screen][privacy]") {
    const auto result = redact_sensitive_response_text(
        R"({"headers":[["Authorization","FF_JSON_PAIR_SECRET"],["Content-Type","application/json"]],"status":"ready"})");
    REQUIRE(result.find("FF_JSON_PAIR_SECRET") == std::string::npos);
    REQUIRE(result.find("Authorization") != std::string::npos);
    REQUIRE(result.find("application/json") != std::string::npos);
    REQUIRE(result.find("ready") != std::string::npos);

    const std::string malformed =
        "{headers:[[Authorization,FF_JSON_PAIR_BROKEN],[Content-Type,application/json]]}";
    REQUIRE(redact_sensitive_response_text(malformed) == "[REDACTED]");
    const std::string ordinary_malformed = "{headers:[[Content-Type,application/json]]}";
    REQUIRE(redact_sensitive_response_text(ordinary_malformed) == ordinary_malformed);
}

TEST_CASE("Response editor suppresses unquoted object pairs from GUI editors", "[screen][privacy]") {
    const std::string live_shape =
        "{FieldName:Authorization,Value:FFLY_FIELDNAME_PAIR_260927_X7}";
    REQUIRE(redact_sensitive_response_text(live_shape) == "[REDACTED]");
    REQUIRE(redact_sensitive_response_text(
        "{HeaderName:client_secret,HeaderValue:FFLY_HEADER_PAIR_260927}") == "[REDACTED]");
    REQUIRE(redact_sensitive_response_text(
        "{access_token:FFLY_DIRECT_KEY_260927}") == "[REDACTED]");
    const std::string ordinary = "{FieldName:Content-Type,Value:application/json}";
    REQUIRE(redact_sensitive_response_text(ordinary) == ordinary);
}

TEST_CASE("Response editor suppresses escaped credential names in malformed object text", "[screen][privacy]") {
    const std::string escaped_name =
        R"({FieldName:Authoriz\u0061tion,Value:FFLY_ESCAPED_PAIR_260927})";
    REQUIRE(redact_sensitive_response_text(escaped_name) == "[REDACTED]");
    REQUIRE(redact_sensitive_response_text(
        R"({access_tok\u0065n:FFLY_ESCAPED_KEY_260927})") == "[REDACTED]");
    REQUIRE(redact_sensitive_response_text(
        R"({"access_tok\u0065n":"FFLY_QUOTED_KEY_260927")") == "[REDACTED]");
    REQUIRE(redact_sensitive_response_text(
        R"({FieldN\u0061me:Authorization,Value:FFLY_ESCAPED_LABEL_260927})") == "[REDACTED]");
    REQUIRE(redact_sensitive_response_text(
        R"({"FieldN\u0061me":"Authorization","Value":"FFLY_QUOTED_LABEL_260927")") == "[REDACTED]");
    const std::string ordinary =
        R"({FieldName:Content-\u0054ype,Value:application/json})";
    REQUIRE(redact_sensitive_response_text(ordinary) == ordinary);
}

TEST_CASE("Response editor masks named credentials in plain text", "[screen][privacy]") {
    const std::string body = "status: ready\r\naccess_token=private-token\r\n"
                             "  client-secret: private-secret\nAuthorization: Bearer private-auth\n"
                             "Content-Type: text/plain\n";
    const auto result = redact_sensitive_response_text(body);
    REQUIRE(result.find("private-token") == std::string::npos);
    REQUIRE(result.find("private-secret") == std::string::npos);
    REQUIRE(result.find("private-auth") == std::string::npos);
    REQUIRE(result.find("status: ready") != std::string::npos);
    REQUIRE(result.find("Content-Type: text/plain") != std::string::npos);
}

TEST_CASE("Response editor masks credential headers on CR-only GUI lines", "[screen][privacy]") {
    const std::string body =
        "HTTP/1.1 200 OK\rAuthorization: Bearer FFLY_CR_ONLY_AUTH_260927\r"
        "Content-Type: application/json\rSet-Cookie: FFLY_CR_ONLY_COOKIE_260927";
    const auto result = redact_sensitive_response_text(body);
    REQUIRE(result.find("FFLY_CR_ONLY_AUTH_260927") == std::string::npos);
    REQUIRE(result.find("FFLY_CR_ONLY_COOKIE_260927") == std::string::npos);
    REQUIRE(result.find("Content-Type: application/json") != std::string::npos);
}

TEST_CASE("Response editor suppresses plain-text values paired with credential names", "[screen][privacy]") {
    const std::string credential_pair =
        "Name: Authorization\rValue: FFLY_PLAIN_PAIR_260927";
    REQUIRE(redact_sensitive_response_text(credential_pair) == "[REDACTED]");
    REQUIRE(redact_sensitive_response_text(
        "HeaderName=client_secret\r\nHeaderValue=FFLY_PLAIN_PAIR_CRLF") == "[REDACTED]");

    const std::string ordinary_pair =
        "Name: Content-Type\r\nValue: application/json";
    REQUIRE(redact_sensitive_response_text(ordinary_pair) == ordinary_pair);
}

TEST_CASE("Response editor masks credential query parameters in URLs", "[screen][privacy]") {
    const std::string body =
        "GET /sap/opu/odata?access_token=FF_QUERY_81&$top=1 HTTP/1.1\n"
        "Location: https://bigfox.example/path?client_secret=FF_QUERY_82&mode=read\n";
    const auto result = redact_sensitive_response_text(body);
    REQUIRE(result.find("FF_QUERY_81") == std::string::npos);
    REQUIRE(result.find("FF_QUERY_82") == std::string::npos);
    REQUIRE(result.find("$top=1") != std::string::npos);
    REQUIRE(result.find("mode=read") != std::string::npos);
}

TEST_CASE("Response editor masks credentials in URL fragments", "[screen][privacy]") {
    const std::string body =
        "Location: https://bigfox.example/callback#access_token=FF_FRAGMENT_83&state=ready\n";
    const auto result = redact_sensitive_response_text(body);
    REQUIRE(result.find("FF_FRAGMENT_83") == std::string::npos);
    REQUIRE(result.find("state=ready") != std::string::npos);
}

TEST_CASE("Response editor masks percent-encoded credential parameter names", "[screen][privacy]") {
    const std::string body =
        "GET /sap/opu/odata?access%5Ftoken=FF_ENCODED_01&$top=1 HTTP/1.1\n"
        "Location: https://bigfox.example/callback#client%5Fsecret=FF_ENCODED_02&state=ready\n";
    const auto result = redact_sensitive_response_text(body);
    REQUIRE(result.find("FF_ENCODED_01") == std::string::npos);
    REQUIRE(result.find("FF_ENCODED_02") == std::string::npos);
    REQUIRE(result.find("$top=1") != std::string::npos);
    REQUIRE(result.find("state=ready") != std::string::npos);

    const auto json_result = redact_sensitive_response_text(
        R"({"url":"https://bigfox.example/?%61ccess_token=FF_ENCODED_03&mode=read"})");
    REQUIRE(json_result.find("FF_ENCODED_03") == std::string::npos);
    REQUIRE(json_result.find("mode=read") != std::string::npos);
}

TEST_CASE("Response editor masks semicolon-delimited credential URL parameters", "[screen][privacy]") {
    const std::string body =
        "GET /sap/opu/odata?$top=1;access_token=FF_SEMICOLON_01&mode=read HTTP/1.1\n"
        "Location: https://bigfox.example/callback#state=ready;client_secret=FF_SEMICOLON_02\n";
    const auto result = redact_sensitive_response_text(body);
    REQUIRE(result.find("FF_SEMICOLON_01") == std::string::npos);
    REQUIRE(result.find("FF_SEMICOLON_02") == std::string::npos);
    REQUIRE(result.find("$top=1") != std::string::npos);
    REQUIRE(result.find("mode=read") != std::string::npos);
    REQUIRE(result.find("state=ready") != std::string::npos);
}

TEST_CASE("ABAP source masks credential statements without shifting line numbers", "[screen][privacy][editor]") {
    const std::string source =
        "DATA(lv_password) = 'FF_PAYLOAD_42'.\n"
        "DATA(lv_token) =\n"
        "  'FF_PAYLOAD_43'.\n"
        "DATA(lv_value) = 'FF_PAYLOAD_44'\n"
        "  && lv_client_secret.\n"
        "WRITE 'ordinary output'.";
    const auto redacted = redact_sensitive_abap_source(source);
    REQUIRE(redacted.find("FF_PAYLOAD_42") == std::string::npos);
    REQUIRE(redacted.find("FF_PAYLOAD_43") == std::string::npos);
    REQUIRE(redacted.find("FF_PAYLOAD_44") == std::string::npos);
    REQUIRE(redacted.find("WRITE 'ordinary output'.") != std::string::npos);
    REQUIRE(std::count(redacted.begin(), redacted.end(), '\n') ==
            std::count(source.begin(), source.end(), '\n'));
}

TEST_CASE("ABAP source keeps template and backtick periods inside credential statements", "[screen][privacy][editor]") {
    const std::string source =
        "DATA(lv_value) = |FF_TEMPLATE.51|\n"
        "  && lv_client_secret.\n"
        "DATA(lv_other) = `FF_BACKTICK.52`\n"
        "  && lv_password.\n"
        "WRITE 'ordinary output'.";
    const auto redacted = redact_sensitive_abap_source(source);
    REQUIRE(redacted.find("FF_TEMPLATE.51") == std::string::npos);
    REQUIRE(redacted.find("FF_BACKTICK.52") == std::string::npos);
    REQUIRE(redacted.find("WRITE 'ordinary output'.") != std::string::npos);
    REQUIRE(std::count(redacted.begin(), redacted.end(), '\n') ==
            std::count(source.begin(), source.end(), '\n'));
}

TEST_CASE("ABAP source does not release an unfinished second statement", "[screen][privacy][editor]") {
    const std::string source =
        "WRITE 'ready'. DATA(lv_value) = |FF_SECOND.63|\n"
        "  && lv_password.\n"
        "WRITE 'ordinary output'.";
    const auto redacted = redact_sensitive_abap_source(source);
    REQUIRE(redacted.find("FF_SECOND.63") == std::string::npos);
    REQUIRE(redacted.find("WRITE 'ordinary output'.") != std::string::npos);
    REQUIRE(std::count(redacted.begin(), redacted.end(), '\n') ==
            std::count(source.begin(), source.end(), '\n'));
}

TEST_CASE("ABAP source suppresses an unfinished editor range", "[screen][privacy][editor]") {
    // The credential name may be on line 201, beyond the bounded editor read.
    const std::string source =
        "WRITE 'ordinary output'.\n"
        "DATA(lv_value) = 'FF_BOUNDARY_64' &&\n";
    const auto redacted = redact_sensitive_abap_source(source);
    REQUIRE(redacted.find("WRITE 'ordinary output'.") != std::string::npos);
    REQUIRE(redacted.find("FF_BOUNDARY_64") == std::string::npos);
    REQUIRE(std::count(redacted.begin(), redacted.end(), '\n') ==
            std::count(source.begin(), source.end(), '\n'));
}

TEST_CASE("Unknown GuiShell subtypes are read as metadata, not silently treated as trees", "[screen][shell][err137]") {
    REQUIRE(classify_shell_extraction("Calendar") == ShellExtractionKind::Metadata);
    REQUIRE(classify_shell_extraction("") == ShellExtractionKind::Metadata);
    REQUIRE(classify_shell_extraction("Toolbar") == ShellExtractionKind::Metadata);
    REQUIRE(classify_shell_extraction("TableTreeControl") == ShellExtractionKind::Tree);
    REQUIRE(classify_shell_extraction("Tree") == ShellExtractionKind::Tree);
    REQUIRE(classify_shell_extraction("GridView") == ShellExtractionKind::Grid);
}

TEST_CASE("Unreadable unknown GuiShell renders one explicit line", "[screen][shell][markdown][err137]") {
    renderers::register_all_renderers();
    const nlohmann::json shell = {
        {"id", "wnd[0]/usr/cntlPARAM/shellcont/shell"}, {"type", "GuiShell"},
        {"subtype", "Calendar"}, {"content_available", false}
    };
    const nlohmann::json data = {
        {"title", "Profile"}, {"transaction", "RZ11"},
        {"elements", nlohmann::json::array({shell})},
        {"hierarchy", {{"other", nlohmann::json::array({shell})}}}
    };
    const auto md = fairyfly::cli::ScreenMarkdownFormatter::format(data);
    REQUIRE(md.find("### GuiShell (Calendar) - content unavailable") != std::string::npos);
}

TEST_CASE("GuiShell with extracted content is rendered", "[screen][shell][markdown][err137]") {
    renderers::register_all_renderers();
    const nlohmann::json shell = {
        {"id", "wnd[0]/usr/shell"}, {"type", "GuiShell"}, {"subtype", "Calendar"},
        {"content_available", true}, {"text_content", "Value 42"}
    };
    const nlohmann::json data = {
        {"elements", nlohmann::json::array({shell})},
        {"hierarchy", {{"other", nlohmann::json::array({shell})}}}
    };
    const auto md = fairyfly::cli::ScreenMarkdownFormatter::format(data);
    REQUIRE(md.find("Value 42") != std::string::npos);
    REQUIRE(md.find("content unavailable") == std::string::npos);
}

TEST_CASE("Screen text filter searches grid cells and tree nodes", "[screen][filters][imp005]") {
    using nlohmann::json;
    json data = {
        {"elements", json::array({
            {{"id", "wnd[0]/usr/grid"}, {"type", "GuiShell"}, {"subtype", "GridView"},
             {"table_data", {{"columns", json::array({"Description", "Value"})},
                             {"rows", json::array({
                                 json::array({"Binding", "ZFFLY_BIND_260929"})})}}}},
            {{"id", "wnd[0]/usr/tree"}, {"type", "GuiShell"}, {"subtype", "Tree"},
             {"tree_data", {{"nodes", json::array({
                 {{"text", "Root"}, {"column_values", {{"Result", "zffly_tree"}}}}})}}}},
            {{"id", "wnd[0]/usr/other"}, {"type", "GuiLabel"}, {"text", "Nothing"}}
        })},
        {"element_count", 3}
    };
    cli::ScreenFilterOptions filters;
    filters.text_contains = "zffly";
    cli::apply_screen_filters(data, filters);
    REQUIRE(data.at("elements").size() == 2);
    REQUIRE(data.at("elements").at(0).at("id") == "wnd[0]/usr/grid");
    REQUIRE(data.at("elements").at(1).at("id") == "wnd[0]/usr/tree");
}

TEST_CASE("Screen text filter still matches object-row grids", "[screen][filters][imp005]") {
    using nlohmann::json;
    json data = {
        {"elements", json::array({
            {{"id", "g"}, {"type", "GuiGridView"},
             {"table_data", {{"rows", json::array({{{"Value", "ZFFLY_BIND_260929"}}})}}}}
        })},
        {"element_count", 1}
    };
    cli::ScreenFilterOptions filters;
    filters.text_contains = "ZFFLY";
    cli::apply_screen_filters(data, filters);
    REQUIRE(data.at("elements").size() == 1);
}

TEST_CASE("collapse_label_duplicates drops empty and duplicate selection labels", "[screen][size][imp006]") {
    using nlohmann::json;
    json elements = json::array({
        {{"id", "a"}, {"type", "GuiCTextField"}, {"label", "Program"}},
        {{"id", "b"}, {"type", "GuiLabel"}, {"name", "%_P_PROG_%_APP_%-TEXT"}, {"text", "Program"}},
        {{"id", "c"}, {"type", "GuiLabel"}, {"name", "%_P_X_%_APP_%-TEXT"}, {"text", "  "}},
        {{"id", "d"}, {"type", "GuiLabel"}, {"name", "%_P_Y_%_APP_%-TEXT"}, {"text", "Unique text"}},
        {{"id", "e"}, {"type", "GuiLabel"}, {"name", "lblOther"}, {"text", "Program"}}
    });
    ScreenReader::collapse_label_duplicates(elements);
    REQUIRE(elements.size() == 3);
    REQUIRE(elements.at(0).at("id") == "a");
    REQUIRE(elements.at(1).at("id") == "d");
    REQUIRE(elements.at(2).at("id") == "e");
}

TEST_CASE("Tab selection matches full id or trailing part", "[screen][tabs][imp006]") {
    using nlohmann::json;
    const std::vector<json> tabs = {
        {{"id", "wnd[0]/usr/tabsTS/tabpTAB1"}, {"type", "GuiTab"}},
        {{"id", "wnd[0]/usr/tabsTS/tabpTAB2"}, {"type", "GuiTab"}},
        {{"id", "wnd[0]/usr/tabsTS/tabpTAB22"}, {"type", "GuiTab"}}
    };
    REQUIRE(ScreenReader::select_tabs(tabs, "").size() == 3);
    auto by_suffix = ScreenReader::select_tabs(tabs, "tabpTAB2");
    REQUIRE(by_suffix.size() == 1);
    REQUIRE(by_suffix.at(0).at("id") == "wnd[0]/usr/tabsTS/tabpTAB2");
    REQUIRE(ScreenReader::select_tabs(tabs, "wnd[0]/usr/tabsTS/tabpTAB1").size() == 1);
    REQUIRE(ScreenReader::select_tabs(tabs, "TAB2").empty());
    REQUIRE(ScreenReader::select_tabs(tabs, "missing").empty());
}

TEST_CASE("screen find reports simple text for GuiTab", "[screen][find][imp006]") {
    REQUIRE(find_type_has_simple_text("GuiTab"));
    REQUIRE(find_type_has_simple_text("GuiButton"));
    REQUIRE_FALSE(find_type_has_simple_text("GuiShell"));
    REQUIRE_FALSE(find_type_has_simple_text("GuiUserArea"));
}

TEST_CASE("Screen Markdown shows the status bar message", "[screen][markdown][status_bar]") {
    renderers::register_all_renderers();
    nlohmann::json data = {
        {"title", "Job Overview"}, {"transaction", "SM37"},
        {"hierarchy", nlohmann::json::object()},
        {"status_bar", {{"text", "Job log displayed"}, {"message_type", "S"}}}
    };
    const auto md = fairyfly::cli::ScreenMarkdownFormatter::format(data);
    REQUIRE(md.find("**Status [S]:** Job log displayed") != std::string::npos);

    data.erase("status_bar");
    REQUIRE(fairyfly::cli::ScreenMarkdownFormatter::format(data).find("**Status") == std::string::npos);
}

namespace {

nlohmann::json selection_fixture() {
    using nlohmann::json;
    const std::string usr = "/app/con[0]/ses[0]/wnd[0]/usr/";
    auto carrier = [&](const std::string& name, const std::string& text, const std::string& label = "") {
        json e = {{"id", usr + "txt" + name}, {"name", name}, {"text", text},
                  {"type", "GuiTextField"}, {"changeable", false}, {"visible", false},
                  {"capabilities", {"readable"}}};
        if (!label.empty()) e["label"] = label;
        return e;
    };
    auto field = [&](const std::string& prefix, const std::string& name, const std::string& type,
                     const std::string& label, const std::string& text, const std::string& tip) {
        return json{{"id", usr + prefix + name}, {"name", name}, {"type", type}, {"label", label},
                    {"text", text}, {"tooltip", tip}, {"changeable", true}, {"visible", false},
                    {"capabilities", {"fillable", "readable"}}};
    };
    return json::array({
        carrier("%_P_DTMODE_%_APP_%-TEXT", "Time Restriction Mode         "),
        field("cmb", "P_DTMODE", "GuiComboBox", "Time Restriction Mode", "Selection options", "Mode"),
        carrier("%_S_DATUM_%_APP_%-TEXT", "Date                          "),
        field("ctxt", "S_DATUM-LOW", "GuiCTextField", "Date", "29.09.2026", "System Date"),
        carrier("%_S_DATUM_%_APP_%-TO_TEXT", "to   ", "Date"),
        [&] { auto f = field("ctxt", "S_DATUM-HIGH", "GuiCTextField", "to", "", "System Date");
              f["has_f4_help"] = true; return f; }(),
        carrier("%_S_UZEIT_%_APP_%-TEXT", "Time                          "),
        field("ctxt", "S_UZEIT-LOW", "GuiCTextField", "Time", "00:00:00", "System Time"),
        carrier("%_S_UZEIT_%_APP_%-TO_TEXT", "to   ", "Time"),
        field("ctxt", "S_UZEIT-HIGH", "GuiCTextField", "to", "00:00:00", "System Time"),
        carrier("%_S_ORPHAN_%_APP_%-TEXT", "Orphan caption            "),
        carrier("%_S_EMPTY_%_APP_%-TEXT", "      "),
        carrier("TOD_NUM", "6       Runtime Errors        "),
        {{"id", usr + "btn%_S_DATUM_%_APP_%-VALU_PUSH"}, {"name", "%_S_DATUM_%_APP_%-VALU_PUSH"},
         {"type", "GuiButton"}, {"text", ""}}
    });
}

bool has_selection_name(const nlohmann::json& elements, const std::string& name) {
    for (const auto& e : elements) if (e.value("name", "") == name) return true;
    return false;
}

} // namespace

TEST_CASE("collapse_label_duplicates removes GuiTextField selection label carriers", "[screen][size][err142]") {
    using nlohmann::json;
    json elements = selection_fixture();
    ScreenReader::collapse_label_duplicates(elements);

    REQUIRE_FALSE(has_selection_name(elements, "%_P_DTMODE_%_APP_%-TEXT"));
    REQUIRE_FALSE(has_selection_name(elements, "%_S_DATUM_%_APP_%-TEXT"));
    REQUIRE_FALSE(has_selection_name(elements, "%_S_DATUM_%_APP_%-TO_TEXT"));
    REQUIRE_FALSE(has_selection_name(elements, "%_S_UZEIT_%_APP_%-TEXT"));
    REQUIRE_FALSE(has_selection_name(elements, "%_S_UZEIT_%_APP_%-TO_TEXT"));
    REQUIRE_FALSE(has_selection_name(elements, "%_S_EMPTY_%_APP_%-TEXT"));

    // Uncovered caption and non-carrier elements survive.
    REQUIRE(has_selection_name(elements, "%_S_ORPHAN_%_APP_%-TEXT"));
    REQUIRE(has_selection_name(elements, "TOD_NUM"));
    REQUIRE(has_selection_name(elements, "%_S_DATUM_%_APP_%-VALU_PUSH"));

    // Paired fields keep their label.
    for (const auto& e : elements) {
        const std::string name = e.value("name", "");
        if (name == "S_DATUM-LOW") REQUIRE(e.at("label") == "Date");
        if (name == "S_DATUM-HIGH") {
            REQUIRE(e.at("label") == "Date");
            REQUIRE(e.at("range_part") == "to");
        }
        if (name == "S_UZEIT-LOW") REQUIRE(e.at("label") == "Time");
        if (name == "P_DTMODE") REQUIRE(e.at("label") == "Time Restriction Mode");
    }
    REQUIRE(elements.size() == 8);
}

TEST_CASE("collapse_label_duplicates only trusts labels of sibling fields", "[screen][size][err142]") {
    using nlohmann::json;
    json elements = json::array({
        {{"id", "a"}, {"name", "P_OTHER"}, {"type", "GuiTextField"}, {"label", "Date"}},
        {{"id", "b"}, {"name", "%_S_DATUM_%_APP_%-TEXT"}, {"type", "GuiTextField"}, {"text", "Date   "}},
        {{"id", "c"}, {"name", "S_DATUMX-LOW"}, {"type", "GuiCTextField"}, {"label", "Date"}}
    });
    ScreenReader::collapse_label_duplicates(elements);
    // base S_DATUM has no sibling field labelled Date (P_OTHER / S_DATUMX are other bases).
    REQUIRE(elements.size() == 3);
}

TEST_CASE("Selection screen markdown keeps captions after label collapse", "[screen][markdown][err142]") {
    renderers::register_all_renderers();
    using nlohmann::json;
    auto build = [](const json& elements) {
        json data = {{"title", "ABAP Runtime Errors"}, {"transaction", "ST22"},
                     {"elements", elements}, {"element_count", elements.size()},
                     {"hierarchy", {{"form_fields", elements}}}};
        return fairyfly::cli::ScreenMarkdownFormatter::format(data);
    };
    json collapsed = selection_fixture();
    ScreenReader::collapse_label_duplicates(collapsed);
    const std::string before = build(selection_fixture());
    const std::string after = build(collapsed);
    INFO(after);
    // Captions are rendered from the field label, so collapsing the carrier rows loses none.
    for (const char* label : {"Time Restriction Mode", "Date", "Time"}) {
        REQUIRE(before.find(label) != std::string::npos);
        REQUIRE(after.find(label) != std::string::npos);
    }
    REQUIRE(after.find("**Time Restriction Mode<br>Mode**") != std::string::npos);
    REQUIRE(after.find("**Date<br>System Date**") != std::string::npos);
    REQUIRE(after.find("Date (to) (F4 Search)") != std::string::npos);
    REQUIRE(after.find("**to (F4 Search)") == std::string::npos);
    REQUIRE(after.find("Orphan caption") != std::string::npos);
    REQUIRE(after.find("%_S_DATUM_%_APP_%-TEXT") == std::string::npos);
    REQUIRE(after.size() < before.size());
}

TEST_CASE("id_probe_candidates gates FindById probing by type and child count", "[screen][probe]") {
    const std::string id = "wnd[0]/usr/x";
    auto contains = [](const std::vector<std::string>& v, const std::string& e) {
        return std::find(v.begin(), v.end(), e) != v.end();
    };

    SECTION("types whose Children collection is complete are never probed") {
        for (const char* type : {"GuiToolbar", "GuiTitlebar", "GuiMenubar", "GuiTabStrip",
                                 "GuiBox", "GuiStatusbar", "GuiUserArea", "GuiButton"}) {
            REQUIRE(id_probe_candidates(type, id, 0).empty());
            REQUIRE(id_probe_candidates(type, id, 5).empty());
        }
    }
    SECTION("GuiTab with children is not probed") {
        REQUIRE(id_probe_candidates("GuiTab", id, 2).empty());
    }
    SECTION("shell hosts get the shell/shellcont set regardless of child count") {
        for (const char* type : {"GuiCustomControl", "GuiContainerShell", "GuiSplitterShell",
                                 "GuiSplitterContainer", "GuiDockShell", "GuiContainerCtrl"}) {
            for (int children : {0, 3}) {
                const auto ids = id_probe_candidates(type, id, children);
                REQUIRE(contains(ids, id + "/shell"));
                REQUIRE(contains(ids, id + "/shell[0]"));
                REQUIRE(contains(ids, id + "/shellcont[3]"));
                REQUIRE(contains(ids, id + "/shellcont"));
                REQUIRE_FALSE(contains(ids, id + "/sub[0]"));
                REQUIRE_FALSE(contains(ids, id + "/cntlGRID_CONTAINER"));
            }
        }
    }
    SECTION("subscreen containers are probed only when Children was empty") {
        for (const char* type : {"GuiSimpleContainer", "GuiScrollContainer", "GuiSubScreen", "GuiTab"}) {
            REQUIRE(id_probe_candidates(type, id, 4).empty());
            const auto ids = id_probe_candidates(type, id, 0);
            REQUIRE(contains(ids, id + "/sub[0]"));
            REQUIRE(contains(ids, id + "/cntlIMAGE_CONTAINER"));
            REQUIRE(contains(ids, id + "/cntl[1]"));
            REQUIRE(contains(ids, id + "/ssubSCR_PRESEL"));
            REQUIRE_FALSE(contains(ids, id + "/shell"));
        }
    }
    SECTION("candidates already known to the collector are filtered out") {
        ScreenElementCollector collector;
        collector.add_id(id + "/ssubSCR_PRESEL");
        collector.add_grid_id(id + "/sub[0]");
        const auto ids = id_probe_candidates("GuiSimpleContainer", id, 0, collector);
        REQUIRE_FALSE(contains(ids, id + "/ssubSCR_PRESEL"));
        REQUIRE_FALSE(contains(ids, id + "/sub[0]"));
        REQUIRE(contains(ids, id + "/sub[1]"));
    }
}

TEST_CASE("ScreenElementCollector dedupes grid and tree ids", "[screen][collector]") {
    ScreenElementCollector collector;
    REQUIRE_FALSE(collector.contains("g"));
    collector.add_grid_id("g");
    collector.add_grid_id("g");
    collector.add_tree_id("t");
    collector.add_tree_id("t");
    REQUIRE(collector.get_grid_ids() == std::vector<std::string>{"g"});
    REQUIRE(collector.get_tree_ids() == std::vector<std::string>{"t"});
    REQUIRE(collector.contains("g"));
    REQUIRE(collector.contains("t"));
    REQUIRE_FALSE(collector.contains("other"));
    REQUIRE(collector.add_id("e"));
    REQUIRE_FALSE(collector.add_id("e"));
    collector.set_known_type("e", "GuiTextField");
    REQUIRE(collector.known_type("e") == "GuiTextField");
    REQUIRE(collector.known_type("missing").empty());
}

TEST_CASE("plan_tab_selection selects only when the tab is not current", "[screen][tabs]") {
    REQUIRE(plan_tab_selection("a/tabpX", "a/tabpX") == TabSelectionPlan::AlreadySelected);
    REQUIRE(plan_tab_selection("a/tabpY", "a/tabpX") == TabSelectionPlan::Select);
    REQUIRE(plan_tab_selection("", "a/tabpX") == TabSelectionPlan::Select);
}

TEST_CASE("id_probe_candidates with probe_all restores the exhaustive legacy set", "[screen][probe]") {
    const std::string id = "wnd[0]/usr/x";
    auto contains = [](const std::vector<std::string>& v, const std::string& e) {
        return std::find(v.begin(), v.end(), e) != v.end();
    };
    for (const char* type : {"GuiUserArea", "GuiTabStrip", "GuiBox", "GuiSimpleContainer",
                             "GuiContainerShell"}) {
        for (int children : {0, 5}) {
            const auto ids = id_probe_candidates(type, id, children, true);
            REQUIRE(contains(ids, id + "/shell"));
            REQUIRE(contains(ids, id + "/shell[0]"));
            REQUIRE(contains(ids, id + "/shellcont[3]"));
            REQUIRE(contains(ids, id + "/shellcont"));
            REQUIRE(contains(ids, id + "/sub[0]"));
            REQUIRE(contains(ids, id + "/cntlIMAGE_CONTAINER"));
            REQUIRE(contains(ids, id + "/cntl[1]"));
            REQUIRE(contains(ids, id + "/ssubSCR_PRESEL"));
        }
    }
    // Default (gated) behavior is unchanged.
    REQUIRE(id_probe_candidates("GuiUserArea", id, 0).empty());
    REQUIRE(id_probe_candidates("GuiUserArea", id, 0, false).empty());
    // The collector overload still drops already-known ids.
    ScreenElementCollector collector;
    collector.add_id(id + "/ssubSCR_PRESEL");
    REQUIRE_FALSE(contains(id_probe_candidates("GuiBox", id, 3, collector, true),
                           id + "/ssubSCR_PRESEL"));
}
