// gui_element_fill on a GridView cell (2026-11-08: every ALV cell of ME21N came back as "[REDACTED]"): the cell echoes
// what the grid shows after the fill, redacted only by the rules a screen read of that row applies.
#include <catch2/catch_test_macros.hpp>

#include "include/field_fill_info.h"
#include "include/sensitive_data.h"

#include <string>
#include <vector>

using namespace fairyfly::sap;

namespace {

GridCellProbe me21n_item(const std::string& column, std::vector<std::string> row) {
    GridCellProbe probe;
    probe.column = column;
    probe.columns = {"EBELP", "MATNR", "MENGE", "WERKS", "EEIND"};
    probe.row = std::move(row);
    probe.row_known = true;
    return probe;
}

}  // namespace

TEST_CASE("grid fill: a business cell echoes the value read back from the cell", "[fill][grid][privacy]") {
    const std::vector<std::string> row = {"10", "MAT-0001", "5", "1000", "09.11.2026"};
    const std::vector<std::pair<std::string, std::string>> filled = {
        {"MATNR", "mat-0001"}, {"MENGE", "5"}, {"WERKS", "1000"}, {"EEIND", "9.11.2026"}};
    for (const auto& [column, typed] : filled) {
        INFO(column);
        bool redacted = true;
        const auto echo = grid_fill_value_echo(me21n_item(column, row), typed, redacted);
        CHECK_FALSE(redacted);
        CHECK_FALSE(is_redaction_marker(echo));
        if (column == "MATNR") CHECK(echo == "MAT-0001");  // what the grid shows, not what was sent
        if (column == "EEIND") CHECK(echo == "09.11.2026");
        if (column == "MENGE") CHECK(echo == "5");
        if (column == "WERKS") CHECK(echo == "1000");
    }
}

TEST_CASE("grid fill: the typed value is echoed when the cell was not read back", "[fill][grid][privacy]") {
    GridCellProbe probe = me21n_item("MATNR", {"10", "", "5", "1000", ""});
    probe.row_known = true;
    // the column is missing from the column order: the typed value stands in for it
    probe.column = "TXZ01";
    bool redacted = true;
    CHECK(grid_fill_value_echo(probe, "Screws", redacted) == "Screws");
    CHECK_FALSE(redacted);
    // a long value is truncated like a field echo
    const auto echo = grid_fill_value_echo(probe, std::string(300, 'a'), redacted);
    CHECK(echo == std::string(kFillEchoMaxChars, 'a') + "...");
}

TEST_CASE("grid fill: credential cells stay redacted", "[fill][grid][privacy]") {
    bool redacted = false;
    SECTION("a column named like a credential") {
        GridCellProbe probe;
        probe.column = "PASSWORD";
        probe.columns = {"BNAME", "PASSWORD"};
        probe.row = {"ZUSER", "hunter2"};
        probe.row_known = true;
        const auto echo = grid_fill_value_echo(probe, "hunter2", redacted);
        CHECK(redacted);
        CHECK(echo.find("hunter2") == std::string::npos);
        CHECK(echo.rfind("[REDACTED: ", 0) == 0);
    }
    SECTION("a known secret column (USR02 hash) even when it is not in the column order") {
        GridCellProbe probe;
        probe.column = "BCODE";
        probe.columns = {"BNAME"};
        probe.row = {"ZUSER"};
        probe.row_known = true;
        CHECK(grid_fill_value_echo(probe, "ABCDEF0123", redacted).find("ABCDEF0123") == std::string::npos);
        CHECK(redacted);
    }
    SECTION("the value cell of a NAME/VALUE row whose name is a credential") {
        GridCellProbe probe;
        probe.column = "VALUE";
        probe.columns = {"NAME", "VALUE"};
        probe.row = {"Authorization", "Bearer abc.def"};
        probe.row_known = true;
        const auto echo = grid_fill_value_echo(probe, "Bearer abc.def", redacted);
        CHECK(redacted);
        CHECK(echo.find("abc.def") == std::string::npos);
    }
    SECTION("a row without column titles whose other cell names a credential") {
        GridCellProbe probe;
        probe.column = "Column1";
        probe.columns = {"Column0", "Column1"};
        probe.row = {"client_secret", "s3cr3t"};
        probe.row_known = true;
        CHECK(grid_fill_value_echo(probe, "s3cr3t", redacted).find("s3cr3t") == std::string::npos);
        CHECK(redacted);
    }
    SECTION("a row that could not be read is not echoed (fail closed)") {
        GridCellProbe probe;
        probe.column = "VALUE";
        const auto echo = grid_fill_value_echo(probe, "Bearer abc.def", redacted);
        CHECK(redacted);
        CHECK(echo == redaction_marker(redaction_reason::unverified));
    }
    SECTION("a credential inside the read-back text is masked like any field echo") {
        GridCellProbe probe = me21n_item("MATNR", {"10", "https://h/x?access_token=abc123", "5", "1000", ""});
        const auto echo = grid_fill_value_echo(probe, "x", redacted);
        CHECK(echo.find("abc123") == std::string::npos);
    }
}

TEST_CASE("field fill: a password field stays redacted next to the grid echo", "[fill][grid][privacy]") {
    FieldProbe probe;
    probe.type = "GuiPasswordField";
    probe.id = "wnd[0]/usr/pwdRSYST-BCODE";
    probe.text_after = "hunter2";
    probe.text_after_known = true;
    bool redacted = false;
    const auto echo = fill_value_echo(probe, "hunter2", redacted);
    CHECK(redacted);
    CHECK(echo == redaction_marker(redaction_reason::password_field));
}
