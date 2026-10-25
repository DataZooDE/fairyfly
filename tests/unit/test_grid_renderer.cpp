// GridRenderer: classic-list status pairs and the rotated (transposed) thread table.
#include <catch2/catch_test_macros.hpp>

#include "include/formatters/grid_renderer.h"

#include <sstream>

using fairyfly::cli::GridCell;
using fairyfly::cli::GridRenderer;

namespace {
GridCell cell(int row, int col, std::string text) { return GridCell{std::move(text), "", row, col}; }
bool has(const std::string& text, const std::string& part) { return text.find(part) != std::string::npos; }
}  // namespace

TEST_CASE("grid renderer: aligned labels pair with the closest value row below", "[grid][renderer]") {
    const std::vector<GridCell> cells = {
        cell(1, 0, "System:"), cell(1, 20, "Client:"),
        cell(2, 0, "   "), cell(2, 20, " "),            // a separator row is skipped
        cell(3, 0, "S4H"), cell(3, 20, "100"),
        cell(6, 0, "OLD"), cell(6, 20, "999"),          // farther below: not chosen
    };
    const std::string out = GridRenderer::render_status_section(cells);
    CHECK(has(out, "## Status Information"));
    CHECK(has(out, "**System**"));
    CHECK(has(out, "S4H"));
    CHECK(has(out, "100"));
    CHECK_FALSE(has(out, "OLD"));
}

TEST_CASE("grid renderer: a missing value is a dash; prose and single labels are no status grid", "[grid][renderer]") {
    const std::string out = GridRenderer::render_status_section(
        {cell(1, 0, "User:"), cell(1, 20, "Date:"), cell(2, 0, "ALICE")});
    CHECK(has(out, "ALICE"));
    CHECK(has(out, "| -"));
    // one label per row (a heading over prose) is not a status grid
    CHECK(GridRenderer::render_status_section({cell(1, 0, "Note:"), cell(2, 0, "text")}).empty());
    CHECK(GridRenderer::render_status_section({cell(1, 0, "a: b"), cell(1, 9, "c: d")}).empty());
    CHECK(GridRenderer::render_status_section({}).empty());
}

TEST_CASE("grid renderer: the rotated table turns each data column into a row", "[grid][renderer]") {
    const std::vector<GridCell> cells = {
        cell(1, 0, " No. "), cell(1, 1, "1"), cell(1, 2, "2"), cell(1, 3, " "),
        cell(2, 0, "Status"), cell(2, 1, "running"), cell(2, 2, "waiting"),
    };
    const std::string out = GridRenderer::render_rotated_table(cells);
    CHECK(has(out, "## Threads"));
    CHECK(has(out, "No."));
    CHECK(has(out, "Status"));
    CHECK(has(out, "running"));
    CHECK(has(out, "waiting"));
    // header, separator and two data rows: the all-blank column 3 is dropped
    std::istringstream lines(out);
    int table_lines = 0;
    for (std::string line; std::getline(lines, line);) table_lines += line.rfind('|', 0) == 0;
    CHECK(table_lines == 4);
}

TEST_CASE("grid renderer: a rotated table needs property names and a data column", "[grid][renderer]") {
    CHECK(GridRenderer::render_rotated_table({}).empty());
    CHECK(GridRenderer::render_rotated_table({cell(1, 0, "No."), cell(2, 0, "Status")}).empty());
    CHECK(GridRenderer::render_rotated_table({cell(1, 0, "  "), cell(1, 1, "x")}).empty());
    CHECK(GridRenderer::render_rotated_table({cell(1, -1, "No."), cell(1, 0, "x")}).empty());
}
