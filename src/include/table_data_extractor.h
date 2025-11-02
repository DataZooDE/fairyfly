#pragma once

#include "com/wrapper.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace fairyfly {
namespace sap {

using json = nlohmann::json;

/// Configuration for table/tree data extraction
struct TableExtractionOptions {
    int max_rows = 20;           // Maximum rows to extract from tables
    int max_tree_depth = 10;     // Maximum depth for tree traversal
    bool include_headers = true; // Extract column headers
    bool include_invisible = false; // Include invisible columns/rows
};

/// Extracted table/grid data structure
struct TableData {
    std::vector<std::string> columns;
    std::vector<std::vector<std::string>> rows;
    int total_row_count = 0;
    int visible_row_count = 0;
};

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

    /// Get cell value from grid at specified position
    std::string get_grid_cell_value(ComGuiElementPtr grid, int row, int col) const;

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
