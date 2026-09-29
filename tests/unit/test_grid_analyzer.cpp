#include <catch2/catch_test_macros.hpp>
#include "include/grid_analyzer.h"
#include "include/formatters/screen_markdown_formatter.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace fairyfly::cli;

TEST_CASE("GridAnalyzer - detect_grid_layout resilience", "[grid][unit]") {
    SECTION("Handles non-array gracefully") {
        REQUIRE_FALSE(GridAnalyzer::detect_grid_layout(json::object()));
        REQUIRE_FALSE(GridAnalyzer::detect_grid_layout(json("a string")));
        REQUIRE_FALSE(GridAnalyzer::detect_grid_layout(nullptr));
    }

    SECTION("Handles array with non-object elements") {
        json elements = json::array({
            "not_an_object",
            123,
            nullptr,
            json::array({1, 2, 3})
        });
        REQUIRE_FALSE(GridAnalyzer::detect_grid_layout(elements));
    }

    SECTION("Detects grid layout with grid coordinates") {
        json elements = json::array({
            {{"id", "/app/wnd[0]/usr/lbl[1,0]"}, {"type", "GuiLabel"}, {"grid_row", 1}, {"grid_col", 0}}
        });
        REQUIRE(GridAnalyzer::detect_grid_layout(elements));
    }
}

TEST_CASE("GridAnalyzer - parse_grid_labels with ID-string children (ERR-015)", "[grid][unit]") {
    SECTION("Parses flat elements with ID-string children without throwing type_error.306") {
        json elements = json::array({
            {
                {"id", "/app/con[0]/ses[0]/wnd[0]/usr"},
                {"type", "GuiUserArea"},
                {"children", json::array({
                    "/app/con[0]/ses[0]/wnd[0]/usr/lbl[1,0]",
                    "/app/con[0]/ses[0]/wnd[0]/usr/lbl[1,10]"
                })}
            },
            {
                {"id", "/app/con[0]/ses[0]/wnd[0]/usr/lbl[1,0]"},
                {"type", "GuiLabel"},
                {"text", "Field Label"},
                {"grid_row", 1},
                {"grid_col", 0}
            },
            {
                {"id", "/app/con[0]/ses[0]/wnd[0]/usr/lbl[1,10]"},
                {"type", "GuiLabel"},
                {"text", "Value 123"},
                {"grid_row", 1},
                {"grid_col", 10}
            }
        });

        std::vector<GridCell> cells;
        REQUIRE_NOTHROW(cells = GridAnalyzer::parse_grid_labels(elements));
        REQUIRE(cells.size() == 2);
        REQUIRE(cells[0].text == "Field Label");
        REQUIRE(cells[0].row == 1);
        REQUIRE(cells[0].col == 0);
        REQUIRE(cells[1].text == "Value 123");
        REQUIRE(cells[1].row == 1);
        REQUIRE(cells[1].col == 10);
    }

    SECTION("Handles mixed array with strings, nulls, and objects") {
        json elements = json::array({
            "string_element",
            nullptr,
            {
                {"id", "/app/usr/lbl[2,0]"},
                {"type", "GuiLabel"},
                {"text", "Valid Label"},
                {"grid_row", 2},
                {"grid_col", 0},
                {"children", json::array({nullptr, "string_child"})}
            }
        });

        std::vector<GridCell> cells;
        REQUIRE_NOTHROW(cells = GridAnalyzer::parse_grid_labels(elements));
        REQUIRE(cells.size() == 1);
        REQUIRE(cells[0].text == "Valid Label");
    }
}

TEST_CASE("ScreenMarkdownFormatter - Grid formatting with ID-string children", "[formatter][markdown][unit]") {
    json screen_data = {
        {"title", "Data Browser: Table TADIR Select Entries 10"},
        {"transaction", "SE16"},
        {"screen_id", "/app/con[0]/ses[0]/wnd[0]"},
        {"element_count", 3},
        {"hierarchy", {
            {"other", json::array({
                {
                    {"id", "/app/con[0]/ses[0]/wnd[0]/mbar/menu[0]"},
                    {"type", "GuiMenu"},
                    {"text", "Table Entry"}
                }
            })},
            {"buttons", json::array()}
        }},
        {"elements", json::array({
            {
                {"id", "/app/con[0]/ses[0]/wnd[0]/usr"},
                {"type", "GuiUserArea"},
                {"children", json::array({
                    "/app/con[0]/ses[0]/wnd[0]/usr/lbl[1,0]",
                    "/app/con[0]/ses[0]/wnd[0]/usr/lbl[1,10]"
                })}
            },
            {
                {"id", "/app/con[0]/ses[0]/wnd[0]/usr/lbl[1,0]"},
                {"type", "GuiLabel"},
                {"text", "PGMID"},
                {"grid_row", 1},
                {"grid_col", 0}
            },
            {
                {"id", "/app/con[0]/ses[0]/wnd[0]/usr/lbl[1,10]"},
                {"type", "GuiLabel"},
                {"text", "R3TR"},
                {"grid_row", 1},
                {"grid_col", 10}
            }
        })}
    };

    std::string md;
    REQUIRE_NOTHROW(md = ScreenMarkdownFormatter::format(screen_data, false));
    REQUIRE_FALSE(md.empty());
    REQUIRE(md.find("Data Browser: Table TADIR") != std::string::npos);
}

TEST_CASE("Screen Markdown includes main ABAP editor source", "[formatter][markdown][editor]") {
    json data = {
        {"title", "ABAP Editor"},
        {"transaction", "SE38"},
        {"screen_id", "wnd[0]"},
        {"hierarchy", {{"other", json::array({
            {{"id", "wnd[0]/usr/editor"}, {"type", "GuiShell"},
             {"subtype", "AbapEditor"},
             {"text_content", "FORM SET_SCREEN_0300.\n  lv_name = iv_name.\nENDFORM."},
             {"source_total_lines", 1187}, {"source_lines_read", 200},
             {"source_truncated", true}}
        })}}}
    };
    const auto markdown = ScreenMarkdownFormatter::format(data, false);
    REQUIRE(markdown.find("FORM SET_SCREEN_0300.") != std::string::npos);
    REQUIRE(markdown.find("200 of 1187") != std::string::npos);
}
