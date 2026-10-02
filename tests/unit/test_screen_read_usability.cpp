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
