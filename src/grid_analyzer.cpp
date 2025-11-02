#include "include/grid_analyzer.h"
#include "include/constants.h"
#include <spdlog/spdlog.h>
#include <unordered_set>
#include <map>
#include <set>
#include <climits>

namespace fairyfly {
namespace cli {

bool GridAnalyzer::detect_grid_layout(const nlohmann::json& elements) {
    if (!elements.is_array()) {
        return false;
    }

    // Check if any element has grid_row and grid_col metadata
    for (const auto& elem : elements) {
        if (elem.contains("grid_row") && elem.contains("grid_col")) {
            return true;
        }
    }

    return false;
}

std::vector<GridCell> GridAnalyzer::parse_grid_labels(const nlohmann::json& elements) {
    std::vector<GridCell> cells;

    if (!elements.is_array()) {
        return cells;
    }

    for (const auto& elem : elements) {
        // Check if this is a GuiLabel with grid coordinates
        if (elem.value("type", "") == "GuiLabel" &&
            elem.contains("grid_row") && elem.contains("grid_col")) {

            std::string text = elem.value("text", "");

            // Skip empty labels
            if (!text.empty() && text.find_first_not_of(" \t\r\n") != std::string::npos) {
                GridCell cell;
                cell.text = text;
                cell.id = elem.value("id", "");
                cell.row = elem["grid_row"].get<int>();
                cell.col = elem["grid_col"].get<int>();
                cells.push_back(cell);
            }
        }

        // Recursively search children for nested labels
        if (elem.contains("children") && elem["children"].is_array()) {
            auto child_cells = parse_grid_labels(elem["children"]);
            cells.insert(cells.end(), child_cells.begin(), child_cells.end());
        }
    }

    return cells;
}

std::vector<GridCell> GridAnalyzer::deduplicate_cells(const std::vector<GridCell>& cells) {
    std::unordered_set<std::string> seen_ids;
    std::vector<GridCell> deduped_cells;
    
    for (const auto& cell : cells) {
        if (!cell.id.empty()) {
            if (seen_ids.insert(cell.id).second) {
                deduped_cells.push_back(cell);
            }
        } else {
            deduped_cells.push_back(cell);
        }
    }
    
    return deduped_cells;
}

std::vector<GridSection> GridAnalyzer::identify_grid_sections(const std::vector<GridCell>& cells) {
    std::vector<GridSection> sections;

    if (cells.empty()) {
        return sections;
    }

    // Build row and column indices
    std::map<int, std::vector<GridCell>> rows_map;
    std::map<int, std::vector<GridCell>> cols_map;

    for (const auto& cell : cells) {
        rows_map[cell.row].push_back(cell);
        cols_map[cell.col].push_back(cell);
    }

    // Detect rotated table: Column 10 often has row headers like "No.", "Thread ID", etc.
    // This is the pattern from SMICM screen
    bool has_rotated_table = false;
    int table_start_row = -1;
    int table_start_col = -1;

    for (const auto& [col, col_cells] : cols_map) {
        // Check if this column has multiple cells with short text (likely headers)
        if (col_cells.size() >= constants::MIN_HEADER_LIKE_CELLS) {
            int header_like = 0;
            for (const auto& cell : col_cells) {
                // Trim text for matching
                std::string trimmed = cell.text;
                trimmed.erase(0, trimmed.find_first_not_of(" \t\r\n"));
                trimmed.erase(trimmed.find_last_not_of(" \t\r\n") + 1);

                // Headers typically end with ":" or are short labels
                if (trimmed.size() < 20 &&
                    (trimmed.find_first_of(":") != std::string::npos ||
                     trimmed == "No." ||
                     trimmed.find("Thread") != std::string::npos ||
                     trimmed.find("Status") != std::string::npos ||
                     trimmed.find("Number") != std::string::npos)) {
                    header_like++;
                }
            }

            if (header_like >= constants::MIN_HEADER_LIKE_CELLS) {
                has_rotated_table = true;
                table_start_col = col;
                table_start_row = col_cells.front().row;
                break;
            }
        }
    }

    // If rotated table detected, create table section
    if (has_rotated_table) {
        GridSection table_section;
        table_section.type = GridSection::RotatedTable;
        table_section.start_row = table_start_row;
        table_section.end_row = rows_map.rbegin()->first;
        table_section.start_col = table_start_col;
        table_section.end_col = cols_map.rbegin()->first;

        // Add all cells from table area
        for (const auto& cell : cells) {
            if (cell.row >= table_section.start_row && cell.col >= table_section.start_col) {
                table_section.cells.push_back(cell);
            }
        }

        sections.push_back(table_section);
    }

    // Detect status section: cells NOT in the table columns
    GridSection status_section;
    status_section.type = GridSection::StatusPairs;
    status_section.start_row = 0;
    status_section.end_row = rows_map.rbegin()->first;
    status_section.start_col = 0;
    status_section.end_col = cols_map.rbegin()->first;

    for (const auto& cell : cells) {
        // Add cells that are in status columns (not table columns)
        // Only exclude from status if we detected a rotated table AND the cell is clearly in the table area
        bool in_table_area = has_rotated_table &&
                           cell.col >= table_start_col &&
                           cell.row >= table_start_row;
        if (!in_table_area) {
            status_section.cells.push_back(cell);
        }
    }

    if (!status_section.cells.empty()) {
        sections.insert(sections.begin(), status_section);
    }

    return sections;
}

} // namespace cli
} // namespace fairyfly

