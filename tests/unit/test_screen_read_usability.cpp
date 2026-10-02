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
