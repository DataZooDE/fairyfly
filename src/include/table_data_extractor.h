#pragma once

#include "com/wrapper.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>
#include <functional>

namespace fairyfly {
namespace sap {

using json = nlohmann::json;

/// Configuration for table/tree data extraction
struct TableExtractionOptions {
    int max_rows = 20;           // Maximum rows to extract from tables
    int row_offset = 0;          // Index of the first row to return (`--offset`)
    int max_tree_depth = 10;     // Maximum depth for tree traversal
    bool include_headers = true; // Extract column headers
    bool include_invisible = false; // Include invisible columns/rows
};

/// Extracted table/grid data structure
struct TableData {
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
    int total_row_count = 0;    // RowCount of the control
    int visible_row_count = 0;  // VisibleRowCount: rows the control shows in its viewport (not rows read)
    int row_offset = 0;         // index of the first returned row
    int empty_rows_trimmed = 0; // trailing all-empty (padding) rows removed from `rows`
};

/// Read a bounded grid using a column order resolved once for the whole grid.
std::vector<std::vector<std::string>> read_grid_rows(
    int row_count, int col_count, int max_rows,
    const std::vector<std::string>& column_names,
    const std::function<std::string(int, const std::string&)>& read_cell,
    int row_offset = 0);

/// Remove trailing rows whose cells are all empty (SAP grids pad their row window with them).
/// Returns how many rows were removed. Empty rows between data rows are kept.
int trim_trailing_empty_rows(std::vector<std::vector<std::string>>& rows);

/// Add the row-window keys to a serialized table: `offset`, `returned`, `total` (= total_row_count),
/// `empty_rows_trimmed`, `next_offset` (when more real rows follow) and `exposed_rows` (when padding was
/// trimmed: the control holds RowCount rows but only this many were readable).
void annotate_row_window(json& table, int offset, int empty_rows_trimmed);

/// Read-only GuiGridView viewport position for validating scroll-dependent data.
json extract_grid_viewport_metadata(ComGuiElementPtr grid);

/// Recover a tree label from item columns when SAP returns empty node text.
std::string recover_tree_node_text(
    const std::string& primary, const std::vector<std::string>& column_names,
    const std::function<std::string(const std::string&)>& read_item);

/// Extracted tree node structure
struct TreeNode {
    std::string text;
    std::string key;
    int level = 0;
    bool expanded = false;
    std::vector<std::string> column_values;  // Values for additional columns
    std::vector<TreeNode> children;
};

/// Extracted tree data structure
struct TreeData {
    std::vector<std::string> columns;
    std::vector<TreeNode> nodes;
};

/// Extracts actual data from SAP GUI table and tree controls
/// Handles GuiTableControl, GuiGridView, and GuiTree elements
class TableDataExtractor {
public:
    TableDataExtractor() = default;
    explicit TableDataExtractor(TableExtractionOptions options);

    /// Extract data from GuiGridView control
    /// Uses RowCount, ColumnCount, and GetCellValue COM API
    TableData extract_grid_data(ComGuiElementPtr element) const;

    /// Extract data from GuiTableControl
    /// Accesses child elements to read cell contents
    TableData extract_table_data(ComGuiElementPtr element) const;

    /// Extract hierarchical data from GuiTree control
    /// Traverses node structure with configurable depth limit
    TreeData extract_tree_data(ComGuiElementPtr element) const;

private:
    TableExtractionOptions options_;

    /// Get column names from grid or table
    std::vector<std::string> get_column_names(ComGuiElementPtr element) const;

    /// Recursively traverse tree nodes
    void traverse_tree_nodes(
        ComGuiElementPtr tree_element,
        std::vector<TreeNode>& nodes,
        int current_depth
    ) const;

    /// Get text for a specific tree node
    std::string get_tree_node_text(ComGuiElementPtr tree, const std::string& node_key) const;
};

} // namespace sap
} // namespace fairyfly
