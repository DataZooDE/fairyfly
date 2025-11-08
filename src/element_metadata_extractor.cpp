#include "include/element_metadata_extractor.h"
#include "include/constants.h"
#include "include/element_type_registry.h"
#include "include/table_data_extractor.h"
#include "include/com/wrapper.h"
#include <spdlog/spdlog.h>

namespace fairyfly {
namespace sap {

// Static cache member definition
std::map<std::string, json> ElementMetadataExtractor::tree_grid_cache_;

void ElementMetadataExtractor::clear_cache() {
    tree_grid_cache_.clear();
}

json ElementMetadataExtractor::extract(ComGuiElementPtr elem, int depth) {
    if (!elem) return nullptr;

    try {
        json metadata;
        std::string elem_id = elem->get_id();
        metadata["id"] = elem_id;
        std::string type = elem->get_type();
        metadata["type"] = type;
        metadata["name"] = elem->get_name();

        // Text content (available on most elements)
        std::string text = elem->get_text();
        metadata["text"] = text;

        // Interactive states - query for all elements (is_enabled handles missing property gracefully)
        bool enabled = elem->is_enabled();
        bool visible = elem->is_visible();
        bool changeable = elem->is_changeable();

        metadata["enabled"] = enabled;
        metadata["visible"] = visible;
        metadata["changeable"] = changeable;

        // Accessibility labels and tooltips - not available on non-visual containers
        if (!is_non_visual_container(type)) {
            std::string label = elem->get_label();
            std::string tooltip = elem->get_tooltip();

            if (!label.empty()) metadata["label"] = label;
            if (!tooltip.empty()) metadata["tooltip"] = tooltip;
        }

        // Container classification
        std::string container_type = elem->get_container_type();
        if (!container_type.empty()) {
            metadata["container_type"] = container_type;
        }

        // Parse grid coordinates for grid-positioned labels (pattern: lbl[row,col])
        if (type == "GuiLabel" && elem_id.find("/lbl[") != std::string::npos) {
            size_t bracket_pos = elem_id.find("/lbl[");
            size_t comma_pos = elem_id.find(",", bracket_pos);
            size_t close_bracket = elem_id.find("]", comma_pos);

            if (comma_pos != std::string::npos && close_bracket != std::string::npos) {
                try {
                    std::string row_str = elem_id.substr(bracket_pos + 5, comma_pos - bracket_pos - 5);
                    std::string col_str = elem_id.substr(comma_pos + 1, close_bracket - comma_pos - 1);

                    metadata["grid_row"] = std::stoi(row_str);
                    metadata["grid_col"] = std::stoi(col_str);
                } catch (const std::invalid_argument&) {
                    // Failed to parse coordinates, skip
                } catch (const std::out_of_range&) {
                    // Number out of range, skip
                }
            }
        }

        // SubType for GuiShell elements (GridView, Tree, Toolbar, etc.)
        if (type == "GuiShell") {
            std::string subtype = elem->get_subtype();
            if (!subtype.empty()) {
                metadata["subtype"] = subtype;
            } else {
                metadata["subtype"] = "N/A";
            }

            // Special handling for GuiShell Toolbar - enumerate buttons via ButtonCount API
            if (subtype == "Toolbar") {
                try {
                    int button_count = elem->get_button_count();
                    spdlog::debug("GuiShell toolbar has {} buttons", button_count);

                    json button_children = json::array();
                    for (int i = 0; i < button_count; ++i) {
                        try {
                            std::string btn_id = elem->get_button_id(i);
                            std::string btn_text = elem->get_button_text(i);
                            std::string btn_tooltip = elem->get_button_tooltip(i);
                            std::string btn_type = elem->get_button_type(i);
                            bool btn_enabled = elem->get_button_enabled(i);

                            // Skip separators
                            if (btn_type == "Separator") continue;

                            // Create synthetic button element
                            json btn_metadata;
                            btn_metadata["id"] = btn_id;
                            btn_metadata["type"] = "GuiButton";
                            btn_metadata["name"] = btn_id;
                            btn_metadata["text"] = btn_text;
                            btn_metadata["tooltip"] = btn_tooltip;
                            btn_metadata["enabled"] = btn_enabled;
                            btn_metadata["button_type"] = btn_type;
                            btn_metadata["changeable"] = true;
                            btn_metadata["visible"] = true;
                            btn_metadata["capabilities"] = json::array({"clickable"});

                            button_children.push_back(btn_metadata);
                        } catch (const std::exception& e) {
                            spdlog::warn("Failed to extract button {} from GuiShell toolbar: {}", i, e.what());
                        }
                    }

                    if (!button_children.empty()) {
                        metadata["children"] = button_children;
                        metadata["child_count"] = button_children.size();
                    }
                } catch (const std::exception& e) {
                    spdlog::debug("GuiShell toolbar button enumeration failed: {}", e.what());
                }
            }
        }

        // Special handling for GuiBox (grouping container)
        if (type == "GuiBox") {
            metadata["is_group"] = true;
        }

        // Derive capabilities
        metadata["capabilities"] = derive_capabilities(type, enabled, changeable);

        // Extract table/tree data for specialized controls
        // Also check if GuiShell contains a tree control based on text (ActiveX ProgID)
        bool is_tree_control = (type == "GuiTree" ||
                               (type == "GuiShell" && text.find("TableTreeControl") != std::string::npos) ||
                               (type == "GuiShell" && text.find("TreeControl") != std::string::npos));

        // Check if GuiShell has SubType=GridView
        std::string subtype = (type == "GuiShell") ? metadata.value("subtype", "") : "";
        bool is_gridview = (type == "GuiGridView" || (type == "GuiShell" && subtype == "GridView"));

        if (is_gridview || type == "GuiTableControl" || is_tree_control) {
            // Check cache first to avoid duplicate expensive extractions
            auto cache_it = tree_grid_cache_.find(elem_id);
            if (cache_it != tree_grid_cache_.end()) {
                // Reuse cached extraction results
                if (cache_it->second.contains("table_data")) {
                    metadata["table_data"] = cache_it->second["table_data"];
                }
                if (cache_it->second.contains("tree_data")) {
                    metadata["tree_data"] = cache_it->second["tree_data"];
                }
                spdlog::debug("Reused cached extraction data for element: {}", elem_id);
            } else {
                // Extract and cache the results
                try {
                    TableExtractionOptions options;
                    options.max_rows = constants::MAX_TABLE_ROWS;
                    options.max_tree_depth = constants::MAX_TREE_DEPTH;
                    options.include_headers = true;

                    TableDataExtractor extractor(options);
                    json cached_data;

                    if (is_gridview) {
                        auto grid_data = extractor.extract_grid_data(elem);
                        if (!grid_data.rows.empty() || !grid_data.columns.empty()) {
                            json table_json;
                            table_json["columns"] = grid_data.columns;
                            table_json["rows"] = grid_data.rows;
                            table_json["total_row_count"] = grid_data.total_row_count;
                            table_json["visible_row_count"] = grid_data.visible_row_count;
                            metadata["table_data"] = table_json;
                            cached_data["table_data"] = table_json;
                            spdlog::debug("Extracted grid data: {} rows × {} columns",
                                         grid_data.rows.size(), grid_data.columns.size());
                        }
                    } else if (type == "GuiTableControl") {
                        auto table_data = extractor.extract_table_data(elem);
                        if (table_data.total_row_count > 0) {
                            json table_json;
                            table_json["total_row_count"] = table_data.total_row_count;
                            metadata["table_data"] = table_json;
                            cached_data["table_data"] = table_json;
                        }
                    } else if (is_tree_control) {
                        auto tree_data = extractor.extract_tree_data(elem);
                        if (!tree_data.nodes.empty()) {
                            json tree_json;
                            tree_json["columns"] = tree_data.columns;

                            // Convert tree nodes to JSON
                            json nodes_array = json::array();
                            std::function<void(const TreeNode&, json&)> convert_node;
                            convert_node = [&](const TreeNode& node, json& node_json) {
                                node_json["text"] = node.text;
                                node_json["key"] = node.key;
                                node_json["level"] = node.level;
                                node_json["expanded"] = node.expanded;
                                if (!node.column_values.empty()) {
                                    node_json["column_values"] = node.column_values;
                                }
                                if (!node.children.empty()) {
                                    json children_array = json::array();
                                    for (const auto& child : node.children) {
                                        json child_json;
                                        convert_node(child, child_json);
                                        children_array.push_back(child_json);
                                    }
                                    node_json["children"] = children_array;
                                }
                            };

                            for (const auto& node : tree_data.nodes) {
                                json node_json;
                                convert_node(node, node_json);
                                nodes_array.push_back(node_json);
                            }

                            tree_json["nodes"] = nodes_array;
                            metadata["tree_data"] = tree_json;
                            cached_data["tree_data"] = tree_json;
                            spdlog::info("Extracted tree data: {} top-level nodes, {} columns",
                                        tree_data.nodes.size(), tree_data.columns.size());
                        }
                    }

                    // Cache the extraction results
                    if (!cached_data.empty()) {
                        tree_grid_cache_[elem_id] = cached_data;
                    }
                } catch (const std::exception& e) {
                    spdlog::warn("Failed to extract table/tree data for {}: {}", type, e.what());
                }
            }
        }

        // Extract text content from GuiShell with AbapEditor subtype (ABAP source code editor)
        if (type == "GuiShell" && subtype == "AbapEditor") {
            try {
                std::string editor_text = elem->get_property_string(L"Text");
                if (!editor_text.empty()) {
                    metadata["text_content"] = editor_text;
                    spdlog::debug("Extracted AbapEditor text content ({} chars)", editor_text.length());
                } else {
                    metadata["text_content"] = nullptr;  // Empty editor
                    spdlog::debug("AbapEditor is empty");
                }
            } catch (const std::exception& e) {
                spdlog::warn("Failed to extract AbapEditor text: {}", e.what());
                metadata["text_content"] = nullptr;
            }
        }

        // Recursively process children (limit depth to MAX_ELEMENT_DEPTH levels)
        if (depth < constants::MAX_ELEMENT_DEPTH) {
            int child_count = elem->get_child_count();

            // Special handling for GuiContainerShell - always try to enumerate children
            bool force_enumerate = (type == "GuiContainerShell" || type == "GuiCustomControl" ||
                                   type == "GuiSplitterContainer" || type == "GuiContainerCtrl" ||
                                   type == "GuiSplitterShell" || type == "GuiDockShell");

            if (child_count > 0 || force_enumerate) {
                spdlog::debug("extract_element_metadata: Processing {} children of {} at depth {} (force={})",
                             child_count, elem->get_id(), depth, force_enumerate);
                json children = json::array();

                try {
                    // Use the .item(index) method which works with SAP GUI collections
                    auto children_collection = elem->children();

                    // For forced enumeration, try up to MAX_CHILDREN_TO_PROCESS items even if child_count is 0
                    int max_children = force_enumerate ?
                        (child_count > 0 ? (std::min)(child_count, constants::MAX_CHILDREN_TO_PROCESS) : constants::MAX_CHILDREN_TO_PROCESS) :
                        (std::min)(child_count, constants::MAX_CHILDREN_TO_PROCESS);

                    for (int i = 0; i < max_children; ++i) {
                        try {
                            ComGuiElementPtr child_ptr = children_collection.item(i);
                            if (!child_ptr) {
                                // If we're force-enumerating and hit null, stop trying
                                if (force_enumerate && child_count == 0) {
                                    spdlog::debug("Force enumerate: Reached end at index {}", i);
                                    break;
                                }
                                spdlog::debug("extract_element_metadata: child {} returned nullptr", i);
                                continue;
                            }

                            json child_data = extract(child_ptr, depth + 1);
                            if (!child_data.is_null()) {
                                children.push_back(child_data);
                            }
                        } catch (const SapGuiException& e) {
                            // Catch most specific exception first
                            if (force_enumerate && child_count == 0) {
                                spdlog::debug("Force enumerate: SapGuiException at index {}, stopping: {}", i, e.what());
                                break;
                            }
                            spdlog::debug("extract_element_metadata: SapGuiException processing child {}: {}", i, e.what());
                        } catch (const ComException& e) {
                            // Catch COM-specific exception second
                            if (force_enumerate && child_count == 0) {
                                spdlog::debug("Force enumerate: ComException at index {}, stopping: {}", i, e.what());
                                break;
                            }
                            spdlog::debug("extract_element_metadata: ComException processing child {}: {}", i, e.what());
                        } catch (const std::exception& e) {
                            // Catch general exception last (base class)
                            if (force_enumerate && child_count == 0) {
                                spdlog::debug("Force enumerate: Exception at index {}, stopping: {}", i, e.what());
                                break;
                            }
                            spdlog::debug("extract_element_metadata: Exception processing child {}: {}", i, e.what());
                        }
                    }
                } catch (const std::exception& e) {
                    spdlog::debug("extract_element_metadata: Exception getting children collection: {}", e.what());
                }

                if (!children.empty()) {
                    metadata["children"] = children;
                    metadata["child_count"] = children.size();
                } else if (child_count > 0) {
                    spdlog::debug("extract_element_metadata: No children extracted for {} (had {} children)",
                                 elem->get_id(), child_count);
                }
            }
        }

        return metadata;
    } catch (const SapGuiException& e) {
        spdlog::debug("extract_element_metadata: SapGuiException extracting metadata: {}", e.what());
        return nullptr;
    } catch (const ComException& e) {
        spdlog::debug("extract_element_metadata: ComException extracting metadata: {}", e.what());
        return nullptr;
    } catch (const std::exception& e) {
        spdlog::debug("extract_element_metadata: std::exception extracting metadata: {}", e.what());
        return nullptr;
    }
}

} // namespace sap
} // namespace fairyfly

