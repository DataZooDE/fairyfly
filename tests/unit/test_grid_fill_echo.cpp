// gui_element_fill on a GridView cell (2026-11-08: every ALV cell of ME21N came back as "[REDACTED]"): the cell echoes
// what the grid shows after the fill, redacted only by the rules a screen read of that row applies.
#include <catch2/catch_test_macros.hpp>

#include "include/field_fill_info.h"
#include "include/sensitive_data.h"

#include <functional>
#include <map>
#include <optional>
#include <set>
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

// ---- the read-back around the edit (fill_grid_cell_echo: fill_grid_cell's whole flow over GridAccess) ---------------

namespace {

/// An in-memory grid behind the GridAccess seam: cells by (row, column), reads that can fail, a log of what was read.
struct MemoryGrid {
    std::vector<std::string> columns;
    std::map<std::string, std::string> titles;
    std::vector<std::map<std::string, std::string>> rows;
    std::set<std::string> failing;  // columns whose GetCellValue fails
    std::vector<std::string> read_log;        // every cell read
    std::vector<std::string> read_before;     // the cells read before the edit
    bool edited = false;
    std::function<void()> after_edit;  // e.g. a re-sort on commit

    GridAccess access(int row, const std::string& column, const std::string& value) {
        GridAccess a;
        a.column_order = [this] { return columns; };
        a.cell = [this](int r, const std::string& c) -> std::optional<std::string> {
            read_log.push_back(c);
            if (!edited) read_before.push_back(c);
            if (failing.count(c) || r < 0 || r >= static_cast<int>(rows.size())) return std::nullopt;
            return rows[static_cast<std::size_t>(r)][c];
        };
        a.title = [this](const std::string& c) -> std::optional<std::string> {
            auto it = titles.find(c);
            return it == titles.end() ? std::nullopt : std::optional<std::string>(it->second);
        };
        a.modify = [this, row, column, value] {
            edited = true;
            rows[static_cast<std::size_t>(row)][column] = value;
            if (after_edit) after_edit();
        };
        return a;
    }
};

MemoryGrid me21n_grid() {
    MemoryGrid g;
    g.columns = {"EBELP", "MATNR", "TXZ01", "MENGE", "MEINS", "WERKS", "EEIND", "NETPR", "WAERS", "BEDNR"};
    g.rows = {{{"EBELP", "10"}, {"MATNR", ""}, {"MENGE", ""}, {"WERKS", ""}},
              {{"EBELP", "20"}, {"MATNR", "MAT-0002"}, {"MENGE", "3"}, {"WERKS", "1000"}}};
    return g;
}

/// rows: [[a, b]] (a braced {{a, b}} would be a JSON object)
nlohmann::json one_row(const std::string& a, const std::string& b) {
    return nlohmann::json::array({nlohmann::json::array({a, b})});
}

MemoryGrid name_value_grid() {
    MemoryGrid g;
    g.columns = {"NAME", "VALUE"};
    g.rows = {{{"NAME", "Authorization"}, {"VALUE", ""}}, {{"NAME", "Content-Type"}, {"VALUE", ""}}};
    return g;
}

}  // namespace

TEST_CASE("grid fill: the filled column reaches the echo decision", "[fill][grid][privacy]") {
    auto g = me21n_grid();
    auto echo = fill_grid_cell_echo(g.access(0, "MATNR", "MAT-0001"), 0, "MATNR", "MAT-0001");
    CHECK(echo.value == "MAT-0001");
    CHECK_FALSE(echo.redacted);
    CHECK(g.rows[0]["MATNR"] == "MAT-0001");  // the edit ran

    g = me21n_grid();
    g.columns.push_back("PASSWORD");
    echo = fill_grid_cell_echo(g.access(0, "PASSWORD", "hunter2"), 0, "PASSWORD", "hunter2");
    CHECK(echo.redacted);
    CHECK(echo.value.find("hunter2") == std::string::npos);
}

TEST_CASE("grid fill: a cell that cannot be read back fails closed", "[fill][grid][privacy]") {
    SECTION("the NAME cell of a NAME/VALUE row") {
        auto g = name_value_grid();
        g.failing = {"NAME"};
        const auto echo = fill_grid_cell_echo(g.access(0, "VALUE", "Bearer abc"), 0, "VALUE", "Bearer abc");
        CHECK(echo.redacted);
        CHECK(echo.value == redaction_marker(redaction_reason::unverified));
    }
    SECTION("the filled cell") {
        auto g = me21n_grid();
        g.failing = {"MATNR"};
        CHECK(fill_grid_cell_echo(g.access(0, "MATNR", "MAT-0001"), 0, "MATNR", "MAT-0001").redacted);
    }
    SECTION("no column order") {
        auto g = me21n_grid();
        g.columns.clear();
        CHECK(fill_grid_cell_echo(g.access(0, "MATNR", "x"), 0, "MATNR", "x").redacted);
    }
    SECTION("a NAME/VALUE row echoes unless its name is a credential") {
        auto g = name_value_grid();
        const auto echo = fill_grid_cell_echo(g.access(1, "VALUE", "application/json"), 1, "VALUE", "application/json");
        CHECK_FALSE(echo.redacted);
        CHECK(echo.value == "application/json");
        g = name_value_grid();
        CHECK(fill_grid_cell_echo(g.access(0, "VALUE", "Bearer abc"), 0, "VALUE", "Bearer abc").redacted);
    }
}

TEST_CASE("grid fill: the anchor before the edit, the row once after it", "[fill][grid][privacy]") {
    auto g = me21n_grid();
    fill_grid_cell_echo(g.access(0, "MENGE", "5"), 0, "MENGE", "5");
    CHECK(g.read_before == std::vector<std::string>{"EBELP"});
    // after the edit every cell of the row exactly once (the screen-read row rule needs all of them)
    std::vector<std::string> after(g.read_log.begin() + static_cast<std::ptrdiff_t>(g.read_before.size()), g.read_log.end());
    std::sort(after.begin(), after.end());
    auto all = g.columns;
    std::sort(all.begin(), all.end());
    CHECK(after == all);
}

TEST_CASE("grid fill: a titled grid without a NAME column hides a credential row like a screen read", "[fill][grid][privacy]") {
    MemoryGrid g;
    g.columns = {"ID", "PARAM", "VALUE"};
    g.rows = {{{"ID", "1"}, {"PARAM", "client_secret"}, {"VALUE", ""}}, {{"ID", "2"}, {"PARAM", "timeout"}, {"VALUE", ""}}};
    // the screen read of that row hides it
    nlohmann::json table = {{"columns", g.columns},
                            {"rows", nlohmann::json::array({nlohmann::json::array({"1", "client_secret", "s3cr3t"})})}};
    redact_sensitive_header_rows(table);
    REQUIRE(table["rows"][0][2] != "s3cr3t");
    // ... and so does the fill echo
    auto echo = fill_grid_cell_echo(g.access(0, "VALUE", "s3cr3t"), 0, "VALUE", "s3cr3t");
    CHECK(echo.redacted);
    CHECK(echo.value.find("s3cr3t") == std::string::npos);
    // a harmless parameter echoes
    echo = fill_grid_cell_echo(g.access(1, "VALUE", "30"), 1, "VALUE", "30");
    CHECK_FALSE(echo.redacted);
    CHECK(echo.value == "30");
}

TEST_CASE("grid fill: without a usable first-column anchor another stable cell anchors the row", "[fill][grid][privacy]") {
    SECTION("the filled column is the first column") {
        auto g = me21n_grid();
        fill_grid_cell_echo(g.access(1, "EBELP", "30"), 1, "EBELP", "30");
        CHECK(g.read_before == std::vector<std::string>{"MATNR"});  // the first non-empty other cell
        g = me21n_grid();
        g.after_edit = [&g] { std::swap(g.rows[0], g.rows[1]); };
        CHECK(fill_grid_cell_echo(g.access(1, "EBELP", "30"), 1, "EBELP", "30").redacted);
    }
    SECTION("the first column is empty") {
        auto g = me21n_grid();
        g.rows[0]["EBELP"] = "";
        g.rows[1]["EBELP"] = "";
        g.rows[0]["MATNR"] = "MAT-0001";
        fill_grid_cell_echo(g.access(0, "MENGE", "5"), 0, "MENGE", "5");
        CHECK(g.read_before == std::vector<std::string>{"EBELP", "MATNR"});
        g.read_log.clear();
        g.read_before.clear();
        g.edited = false;
        g.after_edit = [&g] { std::swap(g.rows[0], g.rows[1]); };
        CHECK(fill_grid_cell_echo(g.access(0, "MENGE", "6"), 0, "MENGE", "6").redacted);
    }
    SECTION("an empty row: echoed unless its content changed") {
        auto g = me21n_grid();
        g.rows.push_back({});  // row 2: every cell empty
        auto echo = fill_grid_cell_echo(g.access(2, "MATNR", "MAT-9"), 2, "MATNR", "MAT-9");
        CHECK_FALSE(echo.redacted);
        CHECK(echo.value == "MAT-9");
        g = me21n_grid();
        g.rows.push_back({});
        g.after_edit = [&g] { std::swap(g.rows[1], g.rows[2]); };  // a filled row moved into row 2
        CHECK(fill_grid_cell_echo(g.access(2, "MATNR", "MAT-9"), 2, "MATNR", "MAT-9").redacted);
    }
}

TEST_CASE("grid fill: a grid that re-sorts after the edit does not echo another row", "[fill][grid][privacy]") {
    auto g = me21n_grid();
    g.after_edit = [&g] { std::swap(g.rows[0], g.rows[1]); };  // commit re-sorted the grid
    const auto echo = fill_grid_cell_echo(g.access(0, "MATNR", "MAT-0001"), 0, "MATNR", "MAT-0001");
    CHECK(echo.redacted);
    CHECK(echo.value == redaction_marker(redaction_reason::unverified));

    auto nv = name_value_grid();
    nv.after_edit = [&nv] { std::swap(nv.rows[0], nv.rows[1]); };
    CHECK(fill_grid_cell_echo(nv.access(1, "VALUE", "x"), 1, "VALUE", "x").redacted);
}

TEST_CASE("grid fill: a displayed column title that names a credential redacts", "[fill][grid][privacy]") {
    for (const char* title : {"Password", "Kennwort", "Passwort", "Kennw.", "Passw."}) {
        INFO(title);
        auto g = me21n_grid();
        g.columns.push_back("ZZFIELD1");
        g.titles["ZZFIELD1"] = title;
        const auto echo = fill_grid_cell_echo(g.access(0, "ZZFIELD1", "hunter2"), 0, "ZZFIELD1", "hunter2");
        CHECK(echo.redacted);
        CHECK(echo.value.find("hunter2") == std::string::npos);
    }
    auto g = me21n_grid();
    g.titles["MATNR"] = "Material";
    CHECK_FALSE(fill_grid_cell_echo(g.access(0, "MATNR", "M-1"), 0, "MATNR", "M-1").redacted);
}

TEST_CASE("credential names: near-miss abbreviations are caught in fill echoes and screen reads", "[fill][grid][privacy][sensitive]") {
    for (const char* name : {"PASSW", "XUBCODE", "KENNW", "RSYST-PASSW", "XU400-XUBCODE", "NEW_PASSW"}) {
        INFO(name);
        // grid fill echo
        GridCellProbe probe;
        probe.column = name;
        probe.columns = {"BNAME", name};
        probe.row = {"ZUSER", "hunter2"};
        probe.row_known = true;
        bool redacted = false;
        CHECK(grid_fill_value_echo(probe, "hunter2", redacted).find("hunter2") == std::string::npos);
        CHECK(redacted);
        // field fill echo
        FieldProbe field;
        field.type = "GuiTextField";
        field.id = std::string("wnd[0]/usr/txt") + name;
        field.text_after = "hunter2";
        field.text_after_known = true;
        CHECK(fill_value_echo(field, "hunter2", redacted).find("hunter2") == std::string::npos);
        // screen read of a grid
        nlohmann::json table = {{"columns", {"BNAME", name}}, {"rows", one_row("ZUSER", "hunter2")}};
        redact_sensitive_header_rows(table);
        CHECK(table["rows"][0][1] != "hunter2");
        // screen read of a NAME/VALUE list
        nlohmann::json list = {{"columns", {"NAME", "VALUE"}}, {"rows", one_row(name, "hunter2")}};
        redact_sensitive_header_rows(list);
        CHECK(list["rows"][0][1] != "hunter2");
    }
    // lookalike business names stay readable
    for (const char* name : {"KENNWERT", "SUBCODE", "BARCODE", "PASSAGE"}) {
        INFO(name);
        nlohmann::json table = {{"columns", {"MATNR", name}}, {"rows", one_row("M-1", "42")}};
        redact_sensitive_header_rows(table);
        CHECK(table["rows"][0][1] == "42");
        GridCellProbe probe;
        probe.column = name;
        probe.columns = {"MATNR", name};
        probe.row = {"M-1", "42"};
        probe.row_known = true;
        bool redacted = true;
        CHECK(grid_fill_value_echo(probe, "42", redacted) == "42");
        CHECK_FALSE(redacted);
    }
}
