#include "include/table_data_extractor.h"
#include <spdlog/spdlog.h>
#include <map>
#include <functional>

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
        // Extract column headers
        if (options_.include_headers) {
            std::string type = element->get_type();
            std::string subtype = element->get_property_string(L"SubType");

            // For GuiShell trees, use ColumnOrder
            if (type == "GuiShell" && subtype == "Tree") {
                auto all_columns = element->get_column_order();
                if (all_columns.size() > 1) {
                    // Skip first column (HierarchyHeader) - it's the text column
                    data.columns = std::vector<std::string>(all_columns.begin() + 1, all_columns.end());
                    spdlog::debug("GuiShell tree has {} data columns", data.columns.size());
                }
            } else {
                // For other tree types, try ColumnCount
                try {
                    int col_count = element->get_property_int(L"ColumnCount");
                    if (col_count > 0) {
                        data.columns = get_column_names(element);
                        spdlog::debug("Tree has {} columns", col_count);
                    }
                } catch (const std::exception&) {
                    // Not a column tree, single column (node text only)
                }
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

            // Call GetCellValue using the ComGuiElement method
            return grid->get_cell_value(row, col_name);
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
        columns = element->get_column_order();

        // If ColumnOrder failed or returned empty, fall back to generic names
        if (columns.empty()) {
            int col_count = element->get_property_int(L"ColumnCount");
            for (int i = 0; i < col_count; ++i) {
                columns.push_back("Column" + std::to_string(i));
            }
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
        std::string type = tree_element->get_type();
        std::string subtype = tree_element->get_property_string(L"SubType");

        // Special handling for GuiShell containing TableTreeControl
        if (type == "GuiShell" && subtype == "Tree") {
            spdlog::debug("Detected GuiShell tree control, using GetAllNodeKeys()");

            // Get all node keys from the tree
            auto node_keys = tree_element->get_all_node_keys();
            spdlog::info("Retrieved {} nodes from GuiShell tree", node_keys.size());

            // Get column names (skip first column which is the hierarchy/text column)
            auto all_columns = tree_element->get_column_order();
            std::vector<std::string> data_columns;
            if (all_columns.size() > 1) {
                data_columns = std::vector<std::string>(all_columns.begin() + 1, all_columns.end());
                spdlog::debug("Tree has {} data columns", data_columns.size());
            }

            // Build flat list with hierarchy information
            std::map<std::string, TreeNode> node_map;  // key -> node

            for (const auto& key : node_keys) {
                TreeNode node;
                node.key = key;
                node.text = tree_element->get_node_text_by_key(key);

                // Get path to determine hierarchy level
                std::string path = tree_element->get_node_path_by_key(key);

                // Parse path to determine level (count backslashes)
                int level = 0;
                for (char c : path) {
                    if (c == '\\') level++;
                }
                node.level = level;

                // Get column values for this node
                for (const auto& col_name : data_columns) {
                    std::string col_value = tree_element->get_item_text(key, col_name);
                    node.column_values.push_back(col_value);
                }

                node.expanded = false;

                // Determine parent from path
                // Path format: "1" (top-level), "1\1" (child of node 1), "1\2\3" (grandchild)
                std::string parent_key;
                if (level > 0) {
                    // Extract parent path by removing last segment
                    size_t last_backslash = path.find_last_of('\\');
                    if (last_backslash != std::string::npos) {
                        std::string parent_path = path.substr(0, last_backslash);

                        // Find node with this path
                        for (const auto& [k, n] : node_map) {
                            if (tree_element->get_node_path_by_key(k) == parent_path) {
                                parent_key = k;
                                break;
                            }
                        }
                    }
                }

                node_map[key] = std::move(node);
            }

            // Build hierarchical structure
            for (auto& [key, node] : node_map) {
                if (node.level == 0) {
                    // Top-level node
                    nodes.push_back(std::move(node));
                } else {
                    // Find parent and add as child
                    std::string path = tree_element->get_node_path_by_key(key);
                    size_t last_backslash = path.find_last_of('\\');
                    if (last_backslash != std::string::npos) {
                        std::string parent_path = path.substr(0, last_backslash);

                        // Find parent node in our tree
                        std::function<bool(std::vector<TreeNode>&, const std::string&, TreeNode&&)> add_to_parent;
                        add_to_parent = [&](std::vector<TreeNode>& tree_nodes, const std::string& target_path, TreeNode&& child) -> bool {
                            for (auto& n : tree_nodes) {
                                std::string n_path = tree_element->get_node_path_by_key(n.key);
                                if (n_path == target_path) {
                                    n.children.push_back(std::move(child));
                                    return true;
                                }
                                if (add_to_parent(n.children, target_path, std::move(child))) {
                                    return true;
                                }
                            }
                            return false;
                        };

                        add_to_parent(nodes, parent_path, std::move(node));
                    }
                }
            }

            return;
        }

        // Original implementation for GuiTree and other tree types
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
                } catch (const std::exception&) {
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
    const std::string& /* node_key */
) const {
    try {
        // Would call GetNodeTextByKey(key) method
        // For now, return empty
        return "";
    } catch (const std::exception& e) {
        spdlog::debug("get_tree_node_text failed: {}", e.what());
        return "";
    }
}

} // namespace sap
} // namespace fairyfly
