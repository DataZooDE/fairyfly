#include "include/table_data_extractor.h"
#include <spdlog/spdlog.h>

namespace fairyfly {
namespace sap {

TableDataExtractor::TableDataExtractor(TableExtractionOptions options)
    : options_(std::move(options))
{
}

TableData TableDataExtractor::extract_grid_data(ComGuiElementPtr element) const {
    TableData data;

    if (!element) {
        spdlog::warn("extract_grid_data: null element");
        return data;
    }

    try {
        // Get row and column counts using COM properties
        int row_count = element->get_property_int(L"RowCount");
        int col_count = element->get_property_int(L"ColumnCount");
        int visible_rows = element->get_property_int(L"VisibleRowCount");

        data.total_row_count = row_count;
        data.visible_row_count = visible_rows;

        spdlog::debug("Grid has {} rows, {} columns ({} visible rows)",
                     row_count, col_count, visible_rows);

        // Extract column headers if requested
        if (options_.include_headers) {
            data.columns = get_column_names(element);
        }

        // Limit rows to extract
        int rows_to_extract = (std::min)(row_count, options_.max_rows);

        // Extract cell values
        for (int row = 0; row < rows_to_extract; ++row) {
            std::vector<std::string> row_data;

            for (int col = 0; col < col_count; ++col) {
                try {
                    std::string cell_value = get_grid_cell_value(element, row, col);
                    row_data.push_back(cell_value);
                } catch (const std::exception& e) {
                    spdlog::debug("Failed to read cell [{},{}]: {}", row, col, e.what());
                    row_data.push_back("");
                }
            }

            data.rows.push_back(row_data);
        }

        spdlog::info("Extracted {} rows × {} columns from grid", data.rows.size(), col_count);

    } catch (const std::exception& e) {
        spdlog::error("extract_grid_data failed: {}", e.what());
    }

    return data;
}

TableData TableDataExtractor::extract_table_data(ComGuiElementPtr element) const {
    TableData data;

    if (!element) {
        spdlog::warn("extract_table_data: null element");
        return data;
    }

    try {
        // GuiTableControl doesn't have GetCellValue - need to access child elements
        // Child elements have IDs like: table_id/row[0]/col[1]

        int child_count = element->get_child_count();
        data.total_row_count = child_count;

        spdlog::debug("Table has {} child elements", child_count);

        // For now, return metadata only
        // Full implementation would require parsing child element IDs
        // to determine row/column structure

    } catch (const std::exception& e) {
        spdlog::error("extract_table_data failed: {}", e.what());
    }

    return data;
}

TreeData TableDataExtractor::extract_tree_data(ComGuiElementPtr element) const {
    TreeData data;

    if (!element) {
        spdlog::warn("extract_tree_data: null element");
        return data;
    }

    try {
        // Extract column headers if tree is a column tree
        if (options_.include_headers) {
            try {
                int col_count = element->get_property_int(L"ColumnCount");
                if (col_count > 0) {
                    data.columns = get_column_names(element);
                    spdlog::debug("Tree has {} columns", col_count);
                }
            } catch (...) {
                // Not a column tree, single column (node text only)
            }
        }

        // Traverse tree nodes recursively
        traverse_tree_nodes(element, data.nodes, 0);

        spdlog::info("Extracted {} top-level nodes from tree", data.nodes.size());

    } catch (const std::exception& e) {
        spdlog::error("extract_tree_data failed: {}", e.what());
    }

    return data;
}

std::string TableDataExtractor::get_grid_cell_value(
    ComGuiElementPtr grid,
    int row,
    int col
) const {
    try {
        // Call GetCellValue method via COM
        // Signature: GetCellValue(row As Long, column As String) As String

        // First, get the column identifier (field name from data dictionary)
        std::vector<std::string> column_names = get_column_names(grid);

        if (col >= 0 && col < static_cast<int>(column_names.size())) {
            std::string col_name = column_names[col];

            // Call GetCellValue using COM method invocation
            // This requires invoking a method with parameters
            // For now, return empty until we implement full COM method invocation

            spdlog::debug("Would call GetCellValue({}, '{}')", row, col_name);
            return "";  // TODO: Implement COM method call
        }

    } catch (const std::exception& e) {
        spdlog::debug("get_grid_cell_value error: {}", e.what());
    }

    return "";
}

std::vector<std::string> TableDataExtractor::get_column_names(ComGuiElementPtr element) const {
    std::vector<std::string> columns;

    try {
        // Try to get ColumnOrder collection (for grids/trees)
        // This is a COM collection object containing column identifiers

        // For now, return generic column names
        // Full implementation would enumerate the ColumnOrder collection

        int col_count = element->get_property_int(L"ColumnCount");
        for (int i = 0; i < col_count; ++i) {
            columns.push_back("Column" + std::to_string(i));
        }

    } catch (const std::exception& e) {
        spdlog::debug("get_column_names error: {}", e.what());
    }

    return columns;
}

void TableDataExtractor::traverse_tree_nodes(
    ComGuiElementPtr tree_element,
    std::vector<TreeNode>& nodes,
    int current_depth
) const {
    if (current_depth >= options_.max_tree_depth) {
        spdlog::debug("Reached max tree depth {}, stopping traversal", options_.max_tree_depth);
        return;
    }

    try {
        // Get tree children (nodes)
        int child_count = tree_element->get_child_count();

        spdlog::debug("Traversing tree at depth {}, {} children", current_depth, child_count);

        // Iterate through child nodes
        for (int i = 0; i < child_count; ++i) {
            try {
                auto child = tree_element->get_child(i);
                if (!child) continue;

                TreeNode node;
                node.level = current_depth;
                node.text = child->get_text();
                node.key = child->get_name();  // Node key

                // Check if node is expanded
                try {
                    node.expanded = child->get_property_bool(L"Expanded");
                } catch (...) {
                    node.expanded = false;
                }

                // Extract column values if this is a column tree
                // Column values would be accessed via GetNodeTextByColumn or similar
                // For now, store just the main text

                // Recursively process child nodes
                if (node.expanded && child->get_child_count() > 0) {
                    traverse_tree_nodes(child, node.children, current_depth + 1);
                }

                nodes.push_back(std::move(node));

            } catch (const std::exception& e) {
                spdlog::debug("Error processing tree node {}: {}", i, e.what());
            }
        }

    } catch (const std::exception& e) {
        spdlog::error("traverse_tree_nodes failed at depth {}: {}", current_depth, e.what());
    }
}

std::string TableDataExtractor::get_tree_node_text(
    ComGuiElementPtr tree,
    const std::string& node_key
) const {
    try {
        // Would call GetNodeTextByKey(key) method
        // For now, return empty
        return "";
    } catch (...) {
        return "";
    }
}

} // namespace sap
} // namespace fairyfly
