// Screen read usability findings of the remote user test: suppressed element
// classes, grid row windows, row filtering, redaction reasons.
#include <catch2/catch_test_macros.hpp>
#include "include/cli_handler.h"
#include "include/element_renderers.h"
#include "include/formatters/screen_markdown_formatter.h"
#include "include/formatters/table_formatter.h"
#include "include/table_data_extractor.h"
#include "include/screen_reader.h"
#include "include/sensitive_data.h"
#include "include/mcp/tool_catalog.h"
#include "include/mcp/types.h"
#include <sstream>

using namespace fairyfly;
using namespace fairyfly::sap;
using nlohmann::json;

TEST_CASE("only=fields reports the grids it suppressed", "[screen][filters][suppressed]") {
    const json grid = {{"id", "wnd[0]/usr/cntlGRID/shellcont/shell"}, {"type", "GuiShell"},
                       {"subtype", "GridView"},
                       {"table_data", {{"columns", json::array({"A"})},
                                       {"rows", json::array({json::array({"x"})})}}}};
    json data = {
        {"elements", json::array({grid})},
        {"hierarchy", {{"tables", json::array({grid})}}},
        {"element_count", 1}
    };

    SECTION("only_fields drops the grid and says so") {
        cli::ScreenFilterOptions filters;
        filters.only_fields = true;
        cli::apply_screen_filters(data, filters);
        REQUIRE(data.at("elements").empty());
        const auto& suppressed = data.at("suppressed");
        REQUIRE(suppressed.at("grids") == 1);
        REQUIRE(suppressed.at("grid_ids").at(0) == "wnd[0]/usr/cntlGRID/shellcont/shell");
        REQUIRE(suppressed.at("by") == "fields");
        REQUIRE(suppressed.at("note").get<std::string>().find("1 grid(s) not included with only=fields") !=
                std::string::npos);
        const auto md = fairyfly::cli::ScreenMarkdownFormatter::format(data);
        REQUIRE(md.find("1 grid(s) not included with only=fields; use only=tables or no filter") !=
                std::string::npos);
    }

    SECTION("only_tables keeps the grid and reports nothing") {
        cli::ScreenFilterOptions filters;
        filters.only_tables = true;
        cli::apply_screen_filters(data, filters);
        REQUIRE(data.at("elements").size() == 1);
        REQUIRE_FALSE(data.contains("suppressed"));
    }

    SECTION("a text filter alone reports nothing") {
        cli::ScreenFilterOptions filters;
        filters.text_contains = "nomatch";
        cli::apply_screen_filters(data, filters);
        REQUIRE_FALSE(data.contains("suppressed"));
    }

    SECTION("grids on tabs are aggregated into the top level") {
        data["tabs_content"] = json::array({
            {{"tab_name", "T"},
             {"elements", json::array({
                 {{"id", "tab/grid"}, {"type", "GuiGridView"}, {"table_data", {{"rows", json::array()}}}}})},
             {"element_count", 1}}});
        cli::ScreenFilterOptions filters;
        filters.only_fields = true;
        cli::apply_screen_filters(data, filters);
        REQUIRE(data.at("suppressed").at("grids") == 2);
    }
}

// ---- Item 2: grid row window ------------------------------------------------------------------

TEST_CASE("trailing empty padding rows are trimmed, inner empty rows stay", "[table][grid][window]") {
    std::vector<std::vector<std::string>> rows = {
        {"a", "b"}, {"", ""}, {"c", ""}, {"", ""}, {" ", ""}, {"", ""}};
    REQUIRE(trim_trailing_empty_rows(rows) == 3);
    REQUIRE(rows.size() == 3);
    REQUIRE(rows.at(1).at(0).empty());
    REQUIRE(rows.at(2).at(0) == "c");

    std::vector<std::vector<std::string>> all_empty = {{"", ""}, {"", ""}};
    REQUIRE(trim_trailing_empty_rows(all_empty) == 2);
    REQUIRE(all_empty.empty());

    std::vector<std::vector<std::string>> none;
    REQUIRE(trim_trailing_empty_rows(none) == 0);
}

TEST_CASE("read_grid_rows starts at the offset", "[table][grid][window]") {
    const std::vector<std::string> columns{"A"};
    int reads = 0;
    const auto rows = read_grid_rows(10, 1, 3, columns,
                                     [&](int row, const std::string&) { ++reads; return std::to_string(row); }, 4);
    REQUIRE(rows.size() == 3);
    REQUIRE(rows.at(0).at(0) == "4");
    REQUIRE(rows.at(2).at(0) == "6");
    REQUIRE(reads == 3);

    const auto tail = read_grid_rows(10, 1, 5, columns,
                                     [](int row, const std::string&) { return std::to_string(row); }, 8);
    REQUIRE(tail.size() == 2);
    REQUIRE(read_grid_rows(10, 1, 5, columns, [](int, const std::string&) { return std::string("x"); }, 10).empty());
}

TEST_CASE("annotate_row_window reports offset, next_offset and exposed rows", "[table][grid][window]") {
    SECTION("full window with more rows following") {
        json table = {{"rows", json::array({json::array({"a"}), json::array({"b"})})},
                      {"total_row_count", 10}};
        annotate_row_window(table, 4, 0);
        REQUIRE(table.at("offset") == 4);
        REQUIRE(table.at("returned") == 2);
        REQUIRE(table.at("total") == 10);
        REQUIRE(table.at("empty_rows_trimmed") == 0);
        REQUIRE(table.at("next_offset") == 6);
        REQUIRE_FALSE(table.contains("exposed_rows"));
    }
    SECTION("last window has no next_offset") {
        json table = {{"rows", json::array({json::array({"a"})})}, {"total_row_count", 5}};
        annotate_row_window(table, 4, 0);
        REQUIRE_FALSE(table.contains("next_offset"));
    }
    SECTION("padding trimmed: the control exposes fewer rows than it counts") {
        json table = {{"rows", json::array({json::array({"a"}), json::array({"b"}), json::array({"c"})})},
                      {"total_row_count", 100}};
        annotate_row_window(table, 0, 17);
        REQUIRE(table.at("empty_rows_trimmed") == 17);
        REQUIRE(table.at("exposed_rows") == 3);
        REQUIRE(table.at("total") == 100);
        REQUIRE_FALSE(table.contains("next_offset"));
    }
}

TEST_CASE("positioned-label table rows are windowed with the same keys", "[table][window]") {
    json table = {{"columns", json::array({"A"})},
                  {"rows", json::array({json::array({"1"}), json::array({"2"}), json::array({"3"}),
                                        json::array({"4"})})},
                  {"total_row_count", 4}, {"visible_row_count", 4}};
    ScreenReader::limit_userarea_table_rows(table, 2, 1);
    REQUIRE(table.at("rows").size() == 2);
    REQUIRE(table.at("rows").at(0).at(0) == "2");
    REQUIRE(table.at("offset") == 1);
    REQUIRE(table.at("returned") == 2);
    REQUIRE(table.at("next_offset") == 3);
    REQUIRE(table.at("total_row_count") == 4);
}

TEST_CASE("markdown table header explains the row counters", "[table][window][markdown]") {
    json element = {{"id", "wnd[0]/usr/grid"}, {"type", "GuiShell"}, {"subtype", "GridView"},
                    {"table_data", {{"columns", json::array({"A"})},
                                    {"rows", json::array({json::array({"x"}), json::array({"y"})})},
                                    {"total_row_count", 79}, {"visible_row_count", 3},
                                    {"offset", 10}, {"returned", 2}, {"total", 79},
                                    {"empty_rows_trimmed", 0}, {"next_offset", 12}}}};
    std::ostringstream oss;
    formatters::TableFormatter().format_to_markdown(element, oss);
    const auto md = oss.str();
    CHECK(md.find("**Total Rows** | 79") != std::string::npos);
    CHECK(md.find("**Visible Rows (viewport)** | 3") != std::string::npos);
    CHECK(md.find("**Returned Rows** | 2") != std::string::npos);
    CHECK(md.find("**Offset** | 10") != std::string::npos);
    CHECK(md.find("Showing rows 10-11 of 79; more rows: offset=12 (--offset 12)") != std::string::npos);

    element["table_data"].erase("next_offset");
    element["table_data"]["empty_rows_trimmed"] = 9;
    element["table_data"]["exposed_rows"] = 12;
    element["table_data"]["offset"] = 0;
    std::ostringstream trimmed;
    formatters::TableFormatter().format_to_markdown(element, trimmed);
    CHECK(trimmed.str().find("The control exposes 12 of 79 rows (9 empty padding row(s) trimmed)") !=
          std::string::npos);
}

TEST_CASE("gui_screen_read passes offset and only=tables to the CLI", "[mcp][tools][window]") {
    const auto specs = mcp::read_tool_specs();
    const mcp::ToolSpec* read = nullptr;
    for (const auto& spec : specs) if (spec.def.name == "gui_screen_read") read = &spec;
    REQUIRE(read != nullptr);
    using Argv = std::vector<std::string>;
    CHECK(read->build_argv(json{{"offset", 40}, {"max_rows", 20}, {"only", "tables"}}, mcp::Policy{}) ==
          Argv{"screen", "read", "--only-tables", "--max-rows", "20", "--offset", "40", "--compact", "--output",
               "markdown"});
    // offset 0 is the default and adds nothing
    CHECK(read->build_argv(json{{"offset", 0}}, mcp::Policy{}) ==
          Argv{"screen", "read", "--max-rows", "20", "--compact", "--output", "markdown"});
    CHECK_THROWS_AS(read->build_argv(json{{"offset", -1}}, mcp::Policy{}), std::invalid_argument);
    CHECK_THROWS_AS(read->build_argv(json{{"offset", "2"}}, mcp::Policy{}), std::invalid_argument);
    CHECK(read->def.input_schema.at("properties").contains("offset"));
    CHECK(read->def.input_schema.at("properties").at("only").at("enum").size() == 5);
}

// ---- Item 3: --text-contains filters grid rows ------------------------------------------------

TEST_CASE("text_contains keeps only matching grid rows", "[screen][filters][rows]") {
    const json grid = {{"id", "wnd[0]/usr/grid"}, {"type", "GuiShell"}, {"subtype", "GridView"},
                       {"table_data", {{"columns", json::array({"USER", "STATUS"})},
                                       {"rows", json::array({json::array({"ALICE", "Locked"}),
                                                             json::array({"BOB", "Active"}),
                                                             json::array({"CAROL", "locked"})})},
                                       {"total_row_count", 3}}}};
    json data = {{"elements", json::array({grid})},
                 {"hierarchy", {{"tables", json::array({grid})}}},
                 {"element_count", 1}};
    cli::ScreenFilterOptions filters;
    filters.text_contains = "LOCKED";
    cli::apply_screen_filters(data, filters);

    for (const json* element : {&data.at("elements").at(0), &data.at("hierarchy").at("tables").at(0)}) {
        const auto& table = element->at("table_data");
        REQUIRE(table.at("rows").size() == 2);
        CHECK(table.at("rows").at(0).at(0) == "ALICE");
        CHECK(table.at("rows").at(1).at(0) == "CAROL");
        CHECK(table.at("rows_matched") == 2);
        CHECK(table.at("rows_total") == 3);
        CHECK(table.at("total_row_count") == 3);
    }
    const auto md = fairyfly::cli::ScreenMarkdownFormatter::format(data);
    CHECK(md.find("**Rows Matched** | 2 of 3") != std::string::npos);
    CHECK(md.find("BOB") == std::string::npos);
}

TEST_CASE("text_contains filters object rows and leaves rows alone on a title match", "[screen][filters][rows]") {
    SECTION("object rows") {
        json data = {{"elements", json::array({
            {{"id", "g"}, {"type", "GuiGridView"},
             {"table_data", {{"rows", json::array({{{"Value", "ZFFLY_ONE"}}, {{"Value", "OTHER"}}})}}}}})},
            {"element_count", 1}};
        cli::ScreenFilterOptions filters;
        filters.text_contains = "zffly";
        cli::apply_screen_filters(data, filters);
        const auto& table = data.at("elements").at(0).at("table_data");
        CHECK(table.at("rows").size() == 1);
        CHECK(table.at("rows_total") == 2);
    }
    SECTION("the element's own label matched: rows stay") {
        json data = {{"elements", json::array({
            {{"id", "g"}, {"type", "GuiTableControl"}, {"label", "Locked users"},
             {"table_data", {{"rows", json::array({json::array({"a"}), json::array({"b"})})}}}}})},
            {"element_count", 1}};
        cli::ScreenFilterOptions filters;
        filters.text_contains = "locked";
        cli::apply_screen_filters(data, filters);
        const auto& table = data.at("elements").at(0).at("table_data");
        CHECK(table.at("rows").size() == 2);
        CHECK_FALSE(table.contains("rows_matched"));
    }
    SECTION("a column title matched: rows stay") {
        json data = {{"elements", json::array({
            {{"id", "g"}, {"type", "GuiGridView"},
             {"table_data", {{"columns", json::array({"LOCKED"})},
                             {"rows", json::array({json::array({"a"}), json::array({"b"})})}}}}})},
            {"element_count", 1}};
        cli::ScreenFilterOptions filters;
        filters.text_contains = "locked";
        cli::apply_screen_filters(data, filters);
        CHECK(data.at("elements").at(0).at("table_data").at("rows").size() == 2);
    }
    SECTION("non-table elements are unchanged") {
        json data = {{"elements", json::array({
            {{"id", "l"}, {"type", "GuiLabel"}, {"text", "Locked"}},
            {{"id", "m"}, {"type", "GuiLabel"}, {"text", "Other"}}})},
            {"element_count", 2}};
        cli::ScreenFilterOptions filters;
        filters.text_contains = "locked";
        cli::apply_screen_filters(data, filters);
        REQUIRE(data.at("elements").size() == 1);
        CHECK(data.at("elements").at(0).at("id") == "l");
    }
}

// ---- Item 4: redaction says why and spares non-secret Basis data ------------------------------

TEST_CASE("redaction markers carry a fixed reason", "[privacy][redaction]") {
    json table = {{"columns", json::array({"USER", "PASSWORD"})},
                  {"rows", json::array({json::array({"alice", "private-password"})})}};
    redact_sensitive_header_rows(table);
    CHECK(table["rows"][0][0] == "alice");
    CHECK(table["rows"][0][1] == "[REDACTED: field name matches password pattern]");

    json secret = {{"columns", json::array({"NAME", "VALUE"})},
                   {"rows", json::array({json::array({"X-Session-Token", "private-value"})})}};
    redact_sensitive_header_rows(secret);
    CHECK(secret["rows"][0][0] == "X-Session-Token");
    CHECK(secret["rows"][0][1] == "[REDACTED: row label names a credential]");

    json columns = {{"columns", json::array({"API_KEY"})},
                    {"rows", json::array({json::array({"private-key"})})}};
    redact_sensitive_header_rows(columns);
    CHECK(columns["rows"][0][0] == "[REDACTED: field name matches secret pattern]");

    // The reason never contains the value or the pattern list.
    const auto text = table.dump() + secret.dump() + columns.dump();
    CHECK(text.find("private") == std::string::npos);
    CHECK(text.find("apikey") == std::string::npos);
    CHECK(is_redaction_marker("[REDACTED]"));
    CHECK(is_redaction_marker("[REDACTED: password input field]"));
    CHECK_FALSE(is_redaction_marker("ordinary"));
}

TEST_CASE("credential state flags, roles and profiles are not redacted", "[privacy][redaction]") {
    json table = {
        {"columns", json::array({"BNAME", "PASSWORD_EXT_PWD_STATE", "AGR_NAME", "PROFILE"})},
        {"rows", json::array({
            json::array({"ALICE", "1", "Z_PASSWORD_RESET_ADMIN", "SAP_BC_SECRET_STORE"}),
            json::array({"BOB", "0", "Z_TOKEN_OPERATOR", "T_TOKEN_PROFILE"})})}};
    const auto before = table;
    redact_sensitive_header_rows(table);
    CHECK(table == before);

    SECTION("role description with a credential word stays readable in a titled grid") {
        json roles = {{"columns", json::array({"AGR_NAME", "TEXT"})},
                      {"rows", json::array({json::array({"Z_PW_ROLE", "Password administration"})})}};
        const auto roles_before = roles;
        redact_sensitive_header_rows(roles);
        CHECK(roles == roles_before);
    }
    SECTION("technical names in an untitled report keep their row") {
        json elements = json::array({
            {{"id", "wnd[0]/usr/lbl[0,0]"}, {"type", "GuiLabel"}, {"text", "Role"}, {"grid_row", 0}},
            {{"id", "wnd[0]/usr/lbl[30,0]"}, {"type", "GuiLabel"}, {"text", "Z_PASSWORD_RESET_ADMIN"},
             {"grid_row", 0}}});
        redact_sensitive_report_labels(elements);
        CHECK(elements[0]["text"] == "Role");
        CHECK(elements[1]["text"] == "Z_PASSWORD_RESET_ADMIN");
    }
    SECTION("input fields named after a state are readable") {
        CHECK_FALSE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtPASSWORD_EXT_PWD_STATE", ""));
        CHECK_FALSE(is_sensitive_input_field("GuiCTextField", "wnd[0]/usr/txtGENERIC", "Password status"));
        CHECK_FALSE(is_sensitive_input_field("GuiTextField", "wnd[0]/usr/txtGENERIC", "Password last changed"));
    }
}

TEST_CASE("secret-bearing fields stay redacted with a reason", "[privacy][redaction]") {
    CHECK(sensitive_input_field_reason("GuiPasswordField", "wnd[0]/usr/pwdRSYST-BCODE", "") ==
          "password input field");
    for (const char* field : {"wnd[0]/usr/txtBAPIPWD", "wnd[0]/usr/ctxtPASSWORD", "wnd[0]/usr/txtNEW_PASSWORD",
                              "wnd[0]/usr/txtUSR02-CODVN", "wnd[0]/usr/txtMY_PASSWORD_FIELD"}) {
        INFO(field);
        CHECK_FALSE(sensitive_input_field_reason("GuiTextField", field, "").empty());
    }
    CHECK(sensitive_input_field_reason("GuiTextField", "wnd[0]/usr/txtPASSWORD", "") ==
          "field name matches password pattern");
    CHECK(sensitive_input_field_reason("GuiTextField", "wnd[0]/usr/txtS_TOKEN", "") ==
          "field name matches secret pattern");
    CHECK(sensitive_input_field_reason("GuiTextField", "wnd[0]/usr/txtGENERIC", "Initial password") ==
          "field name matches password pattern");
    // Authorization metadata is not a credential.
    CHECK(sensitive_input_field_reason("GuiCTextField", "wnd[0]/usr/ctxtX", "Authorization Object").empty());

    // Cells that are a credential name by themselves still hide their row in a business grid.
    json grid = {{"columns", json::array({"FIELD", "CONTENT"})},
                 {"rows", json::array({json::array({"Password", "private-value"}),
                                       json::array({"Description", "visible"})})}};
    redact_sensitive_header_rows(grid);
    CHECK(grid["rows"][0][1] == "[REDACTED: row label names a credential]");
    CHECK(grid["rows"][1][1] == "visible");

    // Untitled grids fail closed for any cell that contains a credential word.
    json untitled = {{"rows", json::array({json::array({"Z_CLIENT_SECRET", "private-value"})})}};
    redact_sensitive_header_rows(untitled);
    CHECK(is_redaction_marker(untitled["rows"][0][1].get<std::string>()));
}

// ---- Item 5: fewer COM round trips -----------------------------------------------------------

TEST_CASE("grid cells are only read when a filter can keep a grid", "[screen][perf]") {
    cli::ScreenFilterOptions none;
    CHECK(cli::screen_filters_need_grid_rows(none));

    cli::ScreenFilterOptions text;
    text.text_contains = "x";
    CHECK(cli::screen_filters_need_grid_rows(text));

    cli::ScreenFilterOptions tables;
    tables.only_tables = true;
    CHECK(cli::screen_filters_need_grid_rows(tables));

    for (auto member : {&cli::ScreenFilterOptions::only_buttons, &cli::ScreenFilterOptions::only_fields,
                        &cli::ScreenFilterOptions::only_editable, &cli::ScreenFilterOptions::only_f4_fields}) {
        cli::ScreenFilterOptions filters;
        filters.*member = true;
        CHECK_FALSE(cli::screen_filters_need_grid_rows(filters));
    }
}

TEST_CASE("a zero row budget reads no grid cells", "[table][grid][perf]") {
    const std::vector<std::string> columns{"A", "B"};
    int reads = 0;
    const auto rows = read_grid_rows(50, 2, 0, columns,
                                     [&](int, const std::string&) { ++reads; return std::string("x"); }, 3);
    CHECK(rows.empty());
    CHECK(reads == 0);
}

TEST_CASE("a grid dropped by an only selector never needed its rows", "[screen][perf][filters]") {
    // The data the reader skips is exactly what the filter discards: the result is identical.
    const json grid = {{"id", "g"}, {"type", "GuiShell"}, {"subtype", "GridView"},
                       {"table_data", {{"columns", json::array({"A"})}, {"rows", json::array()},
                                       {"total_row_count", 5}}},
                       {"toolbar_buttons", json::array({{{"id", "g/btn_X"}, {"type", "GuiButton"}}})}};
    json with_rows = grid;
    with_rows["table_data"]["rows"] = json::array({json::array({"1"}), json::array({"2"})});
    json a = {{"elements", json::array({grid})}, {"element_count", 1}};
    json b = {{"elements", json::array({with_rows})}, {"element_count", 1}};
    cli::ScreenFilterOptions filters;
    filters.only_buttons = true;
    cli::apply_screen_filters(a, filters);
    cli::apply_screen_filters(b, filters);
    CHECK(a.at("elements") == b.at("elements"));
    CHECK(a.at("elements").at(0).at("id") == "g/btn_X");
}
