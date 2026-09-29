#include "include/formatters/grid_renderer.h"
#include "include/formatters/markdown_table_formatter.h"
#include "include/constants.h"
#include <spdlog/spdlog.h>
#include <sstream>
#include <map>
#include <vector>
#include <algorithm>
#include <set>
#include <climits>
#include <limits>

namespace fairyfly {
namespace cli {

std::string GridRenderer::render_status_section(const std::vector<GridCell>& cells) {
    std::ostringstream oss;

    if (cells.empty()) {
        return "";
    }

    // Build map of rows to cells
    std::map<int, std::vector<GridCell>> rows_map;
    for (const auto& cell : cells) {
        rows_map[cell.row].push_back(cell);
    }

    // Sort cells in each row by column
    for (auto& [row, row_cells] : rows_map) {
        std::sort(row_cells.begin(), row_cells.end(),
                  [](const GridCell& a, const GridCell& b) { return a.col < b.col; });
    }

    // Identify actual label rows. Report prose may contain a colon in the
    // middle of a line, but a status property label ends with one.
    std::vector<int> label_rows;
    std::map<int, int> label_to_value_row;  // Maps label row to value row

    for (const auto& [row, row_cells] : rows_map) {
        std::set<int> label_columns;
        for (const auto& cell : row_cells) {
            const auto end = cell.text.find_last_not_of(" \t\r\n");
            if (end != std::string::npos && cell.text[end] == ':') {
                label_columns.insert(cell.col);
            }
        }
        // One-column classic lists often have headings followed by prose. A
        // status grid has multiple aligned property labels in the same row.
        if (label_columns.size() >= 2) {
            label_rows.push_back(row);
        }
    }

    // Find corresponding value rows (row with most matching column positions)
    for (int label_row : label_rows) {
        const auto& label_cells = rows_map[label_row];

        // Build set of label columns
        std::set<int> label_cols;
        for (const auto& cell : label_cells) {
            label_cols.insert(cell.col);
        }

        // Find row with best column overlap, prioritizing closest row below label row
        int best_value_row = -1;
        int best_match_count = 0;
        int best_distance = INT_MAX;

        for (const auto& [row, row_cells] : rows_map) {
            // Skip label row itself
            if (row == label_row) {
                continue;
            }

            // Skip rows that only contain whitespace in the label columns (separator rows)
            bool has_content_in_label_cols = false;
            for (const auto& cell : row_cells) {
                // Only check cells in label columns
                if (label_cols.count(cell.col)) {
                    // Trim and check if there's actual content
                    std::string trimmed = cell.text;
                    trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r"));
                    trimmed.erase(trimmed.find_last_not_of(" \t\n\r") + 1);
                    if (!trimmed.empty()) {
                        has_content_in_label_cols = true;
                        break;
                    }
                }
            }
            if (!has_content_in_label_cols) {
                continue;  // Skip separator/empty rows
            }

            // Count matching columns
            int match_count = 0;
            for (const auto& cell : row_cells) {
                if (label_cols.count(cell.col)) {
                    match_count++;
                }
            }

            // Prefer rows below the label row and closer to it
            // Selection criteria (in order of priority):
            // 1. More matching columns is better
            // 2. If same match count, prefer rows below (row > label_row)
            // 3. If same match count and both below, prefer closer row
            if (match_count > best_match_count) {
                // Strictly better match count
                best_match_count = match_count;
                best_value_row = row;
                best_distance = std::abs(row - label_row);
            } else if (match_count == best_match_count && match_count > 0) {
                // Same match count - apply tiebreaker
                bool new_is_below = (row > label_row);
                bool best_is_below = (best_value_row > label_row);

                if (new_is_below && !best_is_below) {
                    // New row is below, old is above/same - prefer new
                    best_value_row = row;
                    best_distance = std::abs(row - label_row);
                } else if (new_is_below && best_is_below) {
                    // Both below - prefer closer one
                    int new_distance = row - label_row;
                    if (new_distance < best_distance) {
                        best_value_row = row;
                        best_distance = new_distance;
                    }
                }
            }
        }

        if (best_value_row != -1) {
            label_to_value_row[label_row] = best_value_row;
        }
    }

    if (label_to_value_row.empty()) return "";

    // Create table using MarkdownTableFormatter
    formatters::MarkdownTableFormatter table;
    table.add_column("Property", formatters::MarkdownTableFormatter::Alignment::Left);
    table.add_column("Value", formatters::MarkdownTableFormatter::Alignment::Left);

    // Match labels with values column by column
    for (int label_row : label_rows) {
        if (!label_to_value_row.count(label_row)) {
            continue;
        }

        int value_row = label_to_value_row[label_row];
        const auto& label_cells = rows_map[label_row];
        const auto& value_cells = rows_map[value_row];

        // Match cells by column position
        for (const auto& label_cell : label_cells) {
            std::string label = label_cell.text;

            // Remove trailing colon
            if (!label.empty() && label.back() == ':') {
                label = label.substr(0, label.size() - 1);
            }

            // Find value cell in same column
            std::string value = "";
            for (const auto& value_cell : value_cells) {
                if (value_cell.col == label_cell.col) {
                    value = value_cell.text;
                    break;
                }
            }

            if (value.empty()) {
                value = "-";
            }

            // Add row with bold label
            std::vector<std::string> row_data = {label, value};
            std::vector<formatters::MarkdownTableFormatter::CellStyle> styles(2);
            styles[0].bold = true;

            table.add_row(row_data, styles);
        }
    }

    oss << "## Status Information\n\n";
    oss << table.render();
    oss << "\n";

    return oss.str();
}

std::string GridRenderer::render_rotated_table(const std::vector<GridCell>& cells) {
    std::ostringstream oss;

    if (cells.empty()) {
        return "";
    }

    // Build map of rows and columns
    std::map<int, std::map<int, std::string>> grid;
    int min_row = INT_MAX, max_row = INT_MIN;
    int min_col = INT_MAX, max_col = INT_MIN;

    for (const auto& cell : cells) {
        grid[cell.row][cell.col] = cell.text;
        min_row = (std::min)(min_row, cell.row);
        max_row = (std::max)(max_row, cell.row);
        min_col = (std::min)(min_col, cell.col);
        max_col = (std::max)(max_col, cell.col);
    }

    // In the rotated table, column min_col contains property headers (No., Thread ID, etc.)
    // Each data column (min_col+1 to max_col) represents one thread
    // We need to transpose: make each thread a ROW, and properties as COLUMNS

    // Validate grid bounds to prevent overflow
    if (min_col < 0 || max_col < min_col) {
        spdlog::warn("Invalid grid column bounds: min_col={}, max_col={}", min_col, max_col);
        return "";
    }
    if (min_col == std::numeric_limits<int>::max()) {
        spdlog::warn("No grid cells found");
        return "";
    }

    // Extract property names from first column
    std::vector<std::pair<int, std::string>> properties;  // (row, name)
    for (int row = min_row; row <= max_row; ++row) {
        if (grid[row].count(min_col)) {
            std::string prop_name = grid[row][min_col];
            // Trim whitespace
            prop_name.erase(0, prop_name.find_first_not_of(" \t\r\n"));
            prop_name.erase(prop_name.find_last_not_of(" \t\r\n") + 1);
            if (!prop_name.empty()) {
                properties.push_back({row, prop_name});
            }
        }
    }

    if (properties.empty()) {
        return "";
    }

    oss << "## Threads\n\n";

    // Create table using MarkdownTableFormatter
    formatters::MarkdownTableFormatter table;

    // Add columns: one for each property (No., Thread ID, Number, Status, etc.)
    for (const auto& [row, prop_name] : properties) {
        table.add_column(prop_name, formatters::MarkdownTableFormatter::Alignment::Left);
    }

    // Add rows: one row per thread (each data column becomes a row)
    // Validate to prevent integer overflow
    if (min_col == std::numeric_limits<int>::max() || max_col < min_col) {
        return "";
    }
    int start_col = min_col + 1;
    if (start_col > max_col) {
        spdlog::warn("No data columns found (min_col={}, max_col={})", min_col, max_col);
        return "";
    }
    for (int col = start_col; col <= max_col; ++col) {
        std::vector<std::string> row_data;

        // For each property, get the value for this thread
        for (const auto& [prop_row, prop_name] : properties) {
            std::string value = "";
            if (grid[prop_row].count(col)) {
                value = grid[prop_row][col];
            }
            row_data.push_back(value);
        }

        // Skip rows that are all empty
        bool all_empty = true;
        for (const auto& val : row_data) {
            if (!val.empty() && val.find_first_not_of(" \t\r\n") != std::string::npos) {
                all_empty = false;
                break;
            }
        }

        if (!all_empty) {
            // No special styling - regular row
            std::vector<formatters::MarkdownTableFormatter::CellStyle> styles(row_data.size());
            table.add_row(row_data, styles);
        }
    }

    oss << table.render();
    oss << "\n";

    return oss.str();
}

} // namespace cli
} // namespace fairyfly

