#include "include/table_data_extractor.h"
#include <spdlog/spdlog.h>
#include <map>
#include <functional>
#include <algorithm>

namespace fairyfly {
namespace sap {

TableDataExtractor::TableDataExtractor(TableExtractionOptions options)
    : options_(std::move(options))
{
}

std::vector<std::vector<std::string>> read_grid_rows(
    int row_count, int col_count, int max_rows,
    const std::vector<std::string>& column_names,
    const std::function<std::string(int, const std::string&)>& read_cell,
    int row_offset) {
    std::vector<std::vector<std::string>> rows;
    const int first = (std::max)(0, row_offset);
    const int limit = (std::max)(0, (std::min)(row_count, first + (std::max)(0, max_rows)));
    rows.reserve((std::max)(0, limit - first));
    for (int row = first; row < limit; ++row) {
        std::vector<std::string> values;
        values.reserve((std::max)(0, col_count));
        for (int col = 0; col < col_count; ++col) {
            if (col >= static_cast<int>(column_names.size())) {
                values.emplace_back();
                continue;
            }
            try {
                values.push_back(read_cell(row, column_names[col]));
            } catch (const std::exception& error) {
                spdlog::debug("Failed to read grid cell [{},{}]: {}", row, col, error.what());
                values.emplace_back();
            }
        }
        rows.push_back(std::move(values));
    }
    return rows;
}

std::vector<std::vector<std::string>> read_grid_rows_loading(
    int row_count, int col_count, int max_rows,
    const std::vector<std::string>& column_names,
    const std::function<std::string(int, const std::string&)>& read_cell,
    int row_offset, const std::function<void(int)>& scroll_to) {
    auto rows = read_grid_rows(row_count, col_count, max_rows, column_names, read_cell, row_offset);
    if (!scroll_to) return rows;
    const int first = (std::max)(0, row_offset);
    const auto blank_row = [](const std::vector<std::string>& values) {
        return std::all_of(values.begin(), values.end(), [](const std::string& v) {
            return v.find_first_not_of(" \t\r\n") == std::string::npos;
        });
    };
    const auto read_row = [&](int row) {
        std::vector<std::string> values;
        values.reserve((std::max)(0, col_count));
        for (int col = 0; col < col_count; ++col) {
            if (col >= static_cast<int>(column_names.size())) {
                values.emplace_back();
                continue;
            }
            try {
                values.push_back(read_cell(row, column_names[col]));
            } catch (const std::exception&) {
                values.emplace_back();
            }
        }
        return values;
    };
    constexpr int kMaxScrolls = 40;
    for (int scrolls = 0; scrolls < kMaxScrolls; ++scrolls) {
        size_t blank_from = rows.size();
        while (blank_from > 0 && blank_row(rows[blank_from - 1])) --blank_from;
        if (blank_from >= rows.size()) break;                        // no trailing blank rows
        const int target = first + static_cast<int>(blank_from);
        if (target >= row_count) break;                              // the grid really ends here
        try {
            scroll_to(target);
        } catch (const std::exception& error) {
            spdlog::debug("Grid scroll to row {} failed: {}", target, error.what());
            break;
        }
        for (size_t i = blank_from; i < rows.size(); ++i) rows[i] = read_row(first + static_cast<int>(i));
        if (blank_row(rows[blank_from])) break;                      // nothing loaded: stop chasing
    }
    return rows;
}

std::vector<ComGuiElementPtr> enumerate_collection(const SapGuiCollection<ComGuiElement>& collection,
                                                    int limit) {
    std::vector<ComGuiElementPtr> items;
    if (limit <= 0) return items;
    bool walked = false;
    try {
        walked = collection.for_each([&](const ComGuiElementPtr& child) {
            if (static_cast<int>(items.size()) >= limit) return false;
            items.push_back(child);
            return true;
        });
    } catch (const std::exception&) {
        walked = false;
    }
    if (walked && !items.empty()) return items;
    // Discard a partial prefix of a failed walk, then index.
    items.clear();
    const int count = (std::min)(collection.count(), limit);
    for (int i = 0; i < count; ++i) {
        auto item = collection.item(i);
        if (!item) break;
        items.push_back(std::move(item));
    }
    return items;
}

namespace {
bool cell_is_blank(const std::string& value) {
    return value.find_first_not_of(" \t\r\n") == std::string::npos;
}
}  // namespace

int trim_trailing_empty_rows(std::vector<std::vector<std::string>>& rows) {
    size_t keep = rows.size();
    while (keep > 0 && std::all_of(rows[keep - 1].begin(), rows[keep - 1].end(), cell_is_blank)) --keep;
    const int trimmed = static_cast<int>(rows.size() - keep);
    rows.resize(keep);
    return trimmed;
}

void annotate_row_window(json& table, int offset, int empty_rows_trimmed) {
    if (!table.is_object()) return;
    const int returned = table.contains("rows") && table["rows"].is_array()
        ? static_cast<int>(table["rows"].size()) : 0;
    const int total = table.value("total_row_count", returned);
    table["offset"] = offset;
    table["returned"] = returned;
    table["total"] = total;
    table["empty_rows_trimmed"] = empty_rows_trimmed;
    if (empty_rows_trimmed > 0) {
        table["exposed_rows"] = offset + returned;
    } else if (offset + returned < total) {
        table["next_offset"] = offset + returned;
    }
}

std::string recover_tree_node_text(
    const std::string& primary, const std::vector<std::string>& column_names,
    const std::function<std::string(const std::string&)>& read_item) {
    if (!primary.empty()) return primary;
    for (const auto& name : column_names) {
        if (name == "TEXT") {
            const auto value = read_item(name);
            if (!value.empty()) return value;
            break;
        }
    }
    for (const auto& name : column_names) {
        if (name.empty() || name == "TEXT") continue;
        const auto value = read_item(name);
        if (!value.empty()) return value;
    }
    return {};
}

json extract_grid_viewport_metadata(ComGuiElementPtr grid) {
    json viewport = json::object();
    if (!grid) return viewport;
    const auto first_column = grid->get_property_string(L"FirstVisibleColumn");
    const auto current_column = grid->get_property_string(L"CurrentCellColumn");
    if (first_column.empty() && current_column.empty()) return viewport;
    if (!first_column.empty()) viewport["first_visible_column"] = first_column;
    if (!current_column.empty()) viewport["current_cell_column"] = current_column;
    viewport["first_visible_row"] = grid->get_property_int(L"FirstVisibleRow");
    viewport["current_cell_row"] = grid->get_property_int(L"CurrentCellRow");
    return viewport;
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

        const auto column_names = get_column_names(element);
        if (options_.include_headers) data.columns = column_names;
        // ALV grids load rows lazily around their viewport: scroll to the rows the window needs, then put the
        // original scroll position back (a read must not leave the grid moved).
        int original_first_row = -1;
        bool scrolled = false;
        data.rows = read_grid_rows_loading(
            row_count, col_count, options_.max_rows, column_names,
            [&](int row, const std::string& column) { return element->get_cell_value(row, column); },
            options_.row_offset,
            [&](int row) {
                if (original_first_row < 0) original_first_row = element->get_property_int(L"FirstVisibleRow");
                scrolled = true;
                element->set_int_property(L"FirstVisibleRow", row);
            });
        if (scrolled && original_first_row >= 0) {
            try {
                element->set_int_property(L"FirstVisibleRow", original_first_row);
            } catch (const std::exception& error) {
                spdlog::debug("Could not restore the grid scroll position: {}", error.what());
            }
        }
        data.row_offset = (std::max)(0, options_.row_offset);
        data.empty_rows_trimmed = trim_trailing_empty_rows(data.rows);

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
        int row_count = 0;
        int visible_rows = 0;
        try { row_count = element->get_property_int(L"RowCount"); } catch (...) {}
        try { visible_rows = element->get_property_int(L"VisibleRowCount"); } catch (...) {}

        data.total_row_count = row_count;
        data.visible_row_count = visible_rows;

        // Get column headers from Columns collection
        int col_count = 0;
        try {
            auto cols = element->get_dispatch_property(L"Columns");
            if (cols) {
                _variant_t count_val;
                DISPID count_dispid;
                if (SUCCEEDED(get_dispid_via_typeinfo(cols, L"Count", &count_dispid))) {
                    DISPPARAMS no_params = {nullptr, nullptr, 0, 0};
                    if (SUCCEEDED(cols->Invoke(count_dispid, IID_NULL, LOCALE_USER_DEFAULT,
                                              DISPATCH_PROPERTYGET, &no_params, &count_val, nullptr, nullptr))) {
                        col_count = count_val.intVal;
                        SapGuiCollection<ComGuiElement> col_coll(cols);
                        // One enumeration for all columns; a short walk falls back to item(c)
                        // per index (a missing column then still yields "Col<c>").
                        auto column_items = enumerate_collection(col_coll, col_count);
                        const bool walked_all = static_cast<int>(column_items.size()) == col_count;
                        for (int c = 0; c < col_count; ++c) {
                            auto col_elem = walked_all ? column_items[static_cast<size_t>(c)]
                                                       : col_coll.item(c);
                            std::string title;
                            if (col_elem) {
                                try { title = col_elem->get_property_string(L"Title"); } catch (...) {}
                                if (title.empty()) {
                                    try { title = col_elem->get_property_string(L"Name"); } catch (...) {}
                                }
                            }
                            if (title.empty()) {
                                title = "Col" + std::to_string(c);
                            }
                            data.columns.push_back(title);
                        }
                    }
                }
            }
        } catch (const std::exception& e) {
            spdlog::debug("extract_table_data: Failed to extract columns: {}", e.what());
        }

        // Limit rows to extract
        // Note: For GuiTableControl, visible rows are directly accessible via GetCell
        int max_accessible_rows = visible_rows > 0 ? (std::min)(row_count, visible_rows) : row_count;
        const int first_row = (std::max)(0, options_.row_offset);
        const int last_row = (std::min)(max_accessible_rows, first_row + options_.max_rows);
        data.row_offset = first_row;

        DISPID cell_dispid;
        HRESULT hr_cell = get_dispid_via_typeinfo(element->get_dispatch(), L"GetCell", &cell_dispid);

        if (SUCCEEDED(hr_cell) && col_count > 0) {
            for (int r = first_row; r < last_row; ++r) {
                std::vector<std::string> row_data;
                for (int c = 0; c < col_count; ++c) {
                    try {
                        _variant_t row(r);
                        _variant_t col(c);
                        VARIANT args[2] = {col, row}; // reverse order for DISPPARAMS (arg[0]=col, arg[1]=row)
                        DISPPARAMS params = {args, nullptr, 2, 0};
                        _variant_t cell_result;
                        HRESULT hr = element->get_dispatch()->Invoke(
                            cell_dispid, IID_NULL, LOCALE_USER_DEFAULT,
                            DISPATCH_METHOD, &params, &cell_result, nullptr, nullptr);

                        if (SUCCEEDED(hr) && cell_result.vt == VT_DISPATCH && cell_result.pdispVal) {
                            auto cell_elem = ComGuiElement::create(cell_result.pdispVal);
                            std::string cell_val = cell_elem->get_text();
                            if (cell_val.empty() && cell_elem->get_type() == "GuiCheckBox") {
                                try {
                                    bool sel = cell_elem->get_property_bool(L"Selected");
                                    cell_val = sel ? "X" : " ";
                                } catch (...) {}
                            }

                            row_data.push_back(cell_val);
                        } else {
                            row_data.push_back("");
                        }
                    } catch (...) {
                        row_data.push_back("");
                    }
                }
                data.rows.push_back(row_data);
            }
        }

        data.empty_rows_trimmed = trim_trailing_empty_rows(data.rows);
        spdlog::info("Extracted {} rows × {} columns from GuiTableControl", data.rows.size(), data.columns.size());

    } catch (const std::exception& e) {
        spdlog::error("extract_table_data failed: {}", e.what());
    }

    return data;
}

TreeData TableDataExtractor::extract_tree_data(ComGuiElementPtr element) const {
    spdlog::trace("EXTRACT_TREE_DATA: Starting tree data extraction");
    TreeData data;

    if (!element) {
        spdlog::warn("extract_tree_data: null element");
        return data;
    }

    try {
        spdlog::trace("EXTRACT_TREE_DATA: Getting element type and subtype");
        // Extract column headers
        if (options_.include_headers) {
            std::string type = element->get_type();
            std::string subtype = element->get_property_string(L"SubType");
            spdlog::trace("EXTRACT_TREE_DATA: Type={}, SubType={}", type, subtype);

            // For GuiShell trees, use ColumnOrder
            if (type == "GuiShell" && subtype == "Tree") {
                spdlog::trace("EXTRACT_TREE_DATA: Getting column order");
                auto all_columns = element->get_column_order();
                spdlog::trace("EXTRACT_TREE_DATA: Got {} columns", all_columns.size());
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
    spdlog::trace("TREE_EXTRACT_START: traverse_tree_nodes called at depth {}", current_depth);

    if (current_depth >= options_.max_tree_depth) {
        spdlog::debug("Reached max tree depth {}, stopping traversal", options_.max_tree_depth);
        return;
    }

    if (!tree_element) {
        spdlog::warn("traverse_tree_nodes: null tree_element at depth {}", current_depth);
        return;
    }

    try {
        std::string type = tree_element->get_type();
        spdlog::trace("TREE_EXTRACT: Processing tree type={} at depth={}", type, current_depth);
        std::string subtype = tree_element->get_property_string(L"SubType");
        spdlog::trace("TREE_EXTRACT: Tree subtype={}", subtype);

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

            // Pre-compute paths once to avoid excessive COM calls
            spdlog::trace("TREE_EXTRACT: Pre-computing paths for {} nodes...", node_keys.size());
            std::map<std::string, std::string> key_to_path;
            int path_count = 0;
            for (const auto& key : node_keys) {
                try {
                    spdlog::trace("TREE_EXTRACT: Getting path for node key {} ({}/{})", key, ++path_count, node_keys.size());
                    key_to_path[key] = tree_element->get_node_path_by_key(key);
                } catch (const std::exception& e) {
                    spdlog::warn("Failed to get path for node key {}: {}", key, e.what());
                    key_to_path[key] = "";
                }
            }
            spdlog::trace("TREE_EXTRACT: Paths pre-computed successfully");

            spdlog::trace("TREE_EXTRACT: Processing {} nodes...", node_keys.size());
            int node_count = 0;
            std::vector<std::string> tree_column_names;
            bool tree_column_names_loaded = false;
            for (const auto& key : node_keys) {
                try {
                    spdlog::trace("TREE_EXTRACT: Processing node {}/{}: key={}", ++node_count, node_keys.size(), key);
                    TreeNode node;
                    node.key = key;

                    // Get node text with error handling
                    try {
                        node.text = tree_element->get_node_text_by_key(key);
                        if (node.text.empty() && !tree_column_names_loaded) {
                            tree_column_names = tree_element->get_tree_column_names();
                            tree_column_names_loaded = true;
                        }
                        node.text = recover_tree_node_text(
                            node.text, tree_column_names,
                            [&](const std::string& name) {
                                return tree_element->get_item_text(key, name);
                            });
                        spdlog::trace("TREE_EXTRACT: Got text for node {}", key);
                    } catch (const std::exception& e) {
                        spdlog::warn("Failed to get text for node {}: {}", key, e.what());
                        node.text = "<error>";
                    }

                    // Get path to determine hierarchy level
                    std::string path = key_to_path[key];
                    if (path.empty()) {
                        spdlog::debug("Skipping node {} with empty path", key);
                        continue;
                    }

                    // Parse path to determine level (count backslashes)
                    int level = 0;
                    for (char c : path) {
                        if (c == '\\') level++;
                    }
                    node.level = level;

                    // Get column values for this node with error handling
                    spdlog::trace("TREE_EXTRACT: Getting {} column values for node {}...", data_columns.size(), key);
                    for (const auto& col_name : data_columns) {
                        try {
                            std::string col_value = tree_element->get_item_text(key, col_name);
                            node.column_values.push_back(col_value);
                        } catch (const std::exception& e) {
                            spdlog::debug("Failed to get column {} for node {}: {}", col_name, key, e.what());
                            node.column_values.push_back("");  // Empty value on error
                        }
                    }
                    spdlog::trace("TREE_EXTRACT: Node {} processed successfully", key);

                    node.expanded = false;

                    // Determine parent from path using pre-computed paths
                    // Path format: "1" (top-level), "1\1" (child of node 1), "1\2\3" (grandchild)
                    std::string parent_key;
                    if (level > 0) {
                        // Extract parent path by removing last segment
                        size_t last_backslash = path.find_last_of('\\');
                        if (last_backslash != std::string::npos) {
                            std::string parent_path = path.substr(0, last_backslash);

                            // Find node with this path using pre-computed map
                            for (const auto& [k, p] : key_to_path) {
                                if (p == parent_path) {
                                    parent_key = k;
                                    break;
                                }
                            }
                        }
                    }

                    node_map[key] = std::move(node);
                } catch (const std::exception& e) {
                    spdlog::warn("Failed to process tree node {}: {}", key, e.what());
                    continue;
                }
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

                        // Find parent node in our tree using pre-computed paths
                        std::function<bool(std::vector<TreeNode>&, const std::string&, TreeNode&)> add_to_parent;
                        add_to_parent = [&](std::vector<TreeNode>& tree_nodes, const std::string& target_path, TreeNode& child) -> bool {
                            for (auto& n : tree_nodes) {
                                auto path_it = key_to_path.find(n.key);
                                if (path_it != key_to_path.end() && path_it->second == target_path) {
                                    n.children.push_back(std::move(child));
                                    return true;
                                }
                                if (add_to_parent(n.children, target_path, child)) {
                                    return true;
                                }
                            }
                            return false;
                        };

                        add_to_parent(nodes, parent_path, node);
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
