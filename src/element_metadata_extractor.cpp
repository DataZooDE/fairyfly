#include "include/element_metadata_extractor.h"
#include "include/sensitive_data.h"
#include "include/constants.h"
#include "include/element_type_registry.h"
#include "include/html_viewer_reader.h"
#include "include/table_data_extractor.h"
#include "include/com/wrapper.h"
#include <spdlog/spdlog.h>

namespace fairyfly {
namespace sap {

namespace {
// ActiveX ProgIDs such as "SAP.HTMLControl.1": contains '.', no whitespace.
bool looks_like_prog_id(const std::string& text) {
    return !text.empty() && text.find('.') != std::string::npos &&
           text.find_first_of(" \t\r\n") == std::string::npos;
}
} // namespace

// Static cache member definition
std::map<std::string, json> ElementMetadataExtractor::tree_grid_cache_;
bool ElementMetadataExtractor::skip_trees_ = false;

void ElementMetadataExtractor::clear_cache() {
    tree_grid_cache_.clear();
}

void ElementMetadataExtractor::set_skip_trees(bool skip) {
    skip_trees_ = skip;
    if (skip) {
        spdlog::info("Tree extraction disabled (--skip-trees)");
    }
}

bool ElementMetadataExtractor::get_skip_trees() {
    return skip_trees_;
}

json ElementMetadataExtractor::extract(ComGuiElementPtr elem, int depth) {
    if (!elem) return nullptr;

    try {
        json metadata;

        // CRITICAL: Protect against seg fault from tree controls - wrap ALL property access
        std::string elem_id;
        std::string type;

        try {
            elem_id = elem->get_id();
        } catch (const std::exception& e) {
            spdlog::debug("extract: Skipping element - get_id() failed: {}", e.what());
            return nullptr;
        }

        try {
            type = elem->get_type();
        } catch (const std::exception& e) {
            spdlog::debug("extract: Skipping element {} - get_type() failed: {}", elem_id, e.what());
            return nullptr;
        }

        // Debug logging to identify which element causes hangs
        spdlog::debug("extract: Processing element {} type={} depth={}", elem_id, type, depth);

        // If skip_trees is enabled, completely skip tree controls to avoid segfaults
        // Check type BEFORE calling get_text() which might trigger COM calls
        if (skip_trees_ && type == "GuiTree") {
            spdlog::debug("extract: Skipping GuiTree element {} (--skip-trees enabled)", elem_id);
            return nullptr;
        }

        metadata["id"] = elem_id;
        metadata["type"] = type;

        std::string element_name;
        try {
            element_name = elem->get_name();
            metadata["name"] = element_name;
        } catch (const std::exception& e) {
            spdlog::debug("extract: get_name() failed for {}: {}", elem_id, e.what());
            metadata["name"] = "";
        }

        // Never read password fields into automatic screen output.
        std::string text;
        if (type == "GuiPasswordField") {
            metadata["text"] = "[REDACTED]";
        } else {
            try {
                text = elem->get_text();
                metadata["text"] = text;
            } catch (const std::exception& e) {
                spdlog::debug("extract: get_text() failed for {}: {}", elem_id, e.what());
                metadata["text"] = "";
            }
        }

        // Also skip GuiShell elements that contain tree controls
        if (skip_trees_ && type == "GuiShell" &&
            (text.find("TableTreeControl") != std::string::npos ||
             text.find("TreeControl") != std::string::npos)) {
            spdlog::debug("extract: Skipping GuiShell tree control {} (--skip-trees enabled)", elem_id);
            return nullptr;
        }

        // Interactive states - query based on element type capabilities
        bool enabled = true;
        bool visible = true;
        bool changeable = false;

        // Visual and state properties
        if (type != "GuiLabel" && !is_non_visual_container(type)) {
            enabled = elem->is_enabled();
        }
        visible = elem->is_visible();

        // Only input controls can be changeable
        if (type == "GuiTextField" || type == "GuiCTextField" ||
            type == "GuiPasswordField" || type == "GuiOkCodeField" ||
            type == "GuiComboBox" || type == "GuiComboBoxControl" ||
            type == "GuiCheckBox" || type == "GuiRadioButton") {
            changeable = elem->is_changeable();
        }

        metadata["enabled"] = enabled;
        metadata["visible"] = visible;
        metadata["changeable"] = changeable;
        if (type == "GuiCheckBox" || type == "GuiRadioButton") {
            metadata["selected"] = elem->get_property_bool(L"Selected");
        }

        // Accessibility labels and tooltips - only input controls have associated label
        if (!is_non_visual_container(type)) {
            if (type == "GuiTextField" || type == "GuiCTextField" ||
                type == "GuiPasswordField" || type == "GuiComboBox" ||
                type == "GuiComboBoxControl" || type == "GuiCheckBox" ||
                type == "GuiRadioButton") {
                std::string label = elem->get_label();
                if (!label.empty()) metadata["label"] = label;
            }

            // Tooltips
            if (type == "GuiButton" || type == "GuiTextField" || type == "GuiCTextField" ||
                type == "GuiPasswordField" || type == "GuiTab" || type == "GuiComboBox" ||
                type == "GuiStatusPane") {
                std::string tooltip = elem->get_tooltip();
                if (!tooltip.empty()) metadata["tooltip"] = tooltip;
            }
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
                    // SAP GUI coordinate notation is /lbl[col,row] (column character offset, line row number)
                    std::string col_str = elem_id.substr(bracket_pos + 5, comma_pos - bracket_pos - 5);
                    std::string row_str = elem_id.substr(comma_pos + 1, close_bracket - comma_pos - 1);

                    metadata["grid_col"] = std::stoi(col_str);
                    metadata["grid_row"] = std::stoi(row_str);
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

            if (subtype == "HTMLViewer") {
                // Text is the browser control name, not the displayed page.
                metadata.erase("text");
                const auto page = read_html_viewer_text(elem->get_property_int(L"Handle"));
                metadata["content_available"] = !page.text.empty();
                if (!page.text.empty()) {
                    metadata["text"] = redact_sensitive_response_text(page.text);
                    if (page.truncated) metadata["content_truncated"] = true;
                }
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

                            // Create synthetic button element with full path
                            std::string full_btn_path = elem_id + "/btn_" + btn_id;
                            json btn_metadata;
                            btn_metadata["id"] = full_btn_path;
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

                    // Store synthetic buttons in separate field to avoid recursive processing
                    // These are metadata-only constructs, not real COM elements that can be traversed
                    if (!button_children.empty()) {
                        metadata["toolbar_buttons"] = button_children;
                        metadata["button_count"] = button_children.size();
                    }
                } catch (const std::exception& e) {
                    spdlog::debug("GuiShell toolbar button enumeration failed: {}", e.what());
                }
            }
        }

        // Generic probe for GuiShell subtypes without a dedicated reader (ERR-137):
        // grid-like, HTML/text pattern, then accessibility text.
        bool probe_grid = false;
        if (type == "GuiShell") {
            const std::string shell_subtype = metadata.value("subtype", "");
            const bool known = shell_subtype == "GridView" || shell_subtype == "Tree" ||
                               shell_subtype == "TableTreeControl" || shell_subtype == "Toolbar" ||
                               shell_subtype == "HTMLViewer" || shell_subtype == "AbapEditor" ||
                               shell_subtype == "TextEdit";
            if (!known) {
                try {
                    probe_grid = elem->get_property_int(L"RowCount") > 0 &&
                                 elem->get_property_int(L"ColumnCount") > 0;
                } catch (const std::exception&) {}
                if (!probe_grid) {
                    std::string content;
                    try {
                        const auto page = read_html_viewer_text(elem->get_property_int(L"Handle"), 1);
                        content = page.text;
                        if (!content.empty() && page.truncated) metadata["content_truncated"] = true;
                    } catch (const std::exception&) {}
                    for (const wchar_t* prop : {L"AccText", L"AccDescription"}) {
                        if (!content.empty()) break;
                        try { content = elem->get_property_string(prop); }
                        catch (const std::exception&) {}
                    }
                    if (!content.empty()) {
                        metadata["text_content"] = redact_sensitive_response_text(content);
                    }
                    metadata["content_available"] = !content.empty();
                } else {
                    metadata["content_available"] = true;
                }
                // The Text of a shell is usually its ActiveX ProgID (SAP.HTMLControl.1).
                if (looks_like_prog_id(text)) metadata.erase("text");
            }
        }

        // Special handling for GuiBox (grouping container)
        if (type == "GuiBox") {
            metadata["is_group"] = true;
        }

        // Derive capabilities
        metadata["capabilities"] = derive_capabilities(type, enabled, changeable);

        // Detect F4 search help availability
        // GuiCTextField always has F4 help (the "C" stands for "Combo"/search)
        if (type == "GuiCTextField") {
            metadata["has_f4_help"] = true;
            // Add to capabilities array as well
            if (metadata["capabilities"].is_array()) {
                metadata["capabilities"].push_back("has_f4_help");
            }
        }

        // Extract table/tree data for specialized controls
        // Also check if GuiShell contains a tree control based on text (ActiveX ProgID)
        bool is_tree_control = (type == "GuiTree" ||
                               (type == "GuiShell" && text.find("TableTreeControl") != std::string::npos) ||
                               (type == "GuiShell" && text.find("TreeControl") != std::string::npos));

        // Check if GuiShell has SubType=GridView
        std::string subtype = (type == "GuiShell") ? metadata.value("subtype", "") : "";
        bool is_gridview = (type == "GuiGridView" || probe_grid || (type == "GuiShell" && subtype == "GridView"));

        if (is_gridview || type == "GuiTableControl" || is_tree_control) {
            spdlog::debug("extract: Element {} is gridview={} table={} tree={}", elem_id, is_gridview, type == "GuiTableControl", is_tree_control);

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
                spdlog::debug("extract: Starting table/tree extraction for {}", elem_id);
                try {
                    TableExtractionOptions options;
                    options.max_rows = constants::MAX_TABLE_ROWS;
                    options.max_tree_depth = constants::MAX_TREE_DEPTH;
                    options.include_headers = true;

                    TableDataExtractor extractor(options);
                    json cached_data;

                    if (is_gridview) {
                        spdlog::debug("extract: Calling extract_grid_data for {}", elem_id);
                        auto grid_data = extractor.extract_grid_data(elem);
                        if (!grid_data.rows.empty() || !grid_data.columns.empty()) {
                            json table_json;
                            table_json["columns"] = grid_data.columns;
                            table_json["rows"] = grid_data.rows;
                            table_json["total_row_count"] = grid_data.total_row_count;
                            table_json["visible_row_count"] = grid_data.visible_row_count;
                            redact_sensitive_header_rows(table_json);
                            metadata["table_data"] = table_json;
                            cached_data["table_data"] = table_json;
                            spdlog::debug("Extracted grid data: {} rows × {} columns",
                                         grid_data.rows.size(), grid_data.columns.size());
                        }
                    } else if (type == "GuiTableControl") {
                        auto table_data = extractor.extract_table_data(elem);
                        if (!table_data.rows.empty() || !table_data.columns.empty() || table_data.total_row_count > 0) {
                            json table_json;
                            table_json["columns"] = table_data.columns;
                            table_json["rows"] = table_data.rows;
                            table_json["total_row_count"] = table_data.total_row_count;
                            table_json["visible_row_count"] = table_data.visible_row_count;
                            redact_sensitive_header_rows(table_json);
                            metadata["table_data"] = table_json;
                            cached_data["table_data"] = table_json;
                            spdlog::debug("Extracted table control data: {} rows × {} columns",
                                         table_data.rows.size(), table_data.columns.size());
                        }
                    } else if (is_tree_control && !skip_trees_) {
                        auto tree_data = extractor.extract_tree_data(elem);
                        if (!tree_data.nodes.empty()) {
                            json tree_json;
                            tree_json["columns"] = tree_data.columns;

                            // Convert tree nodes to JSON with depth limit to prevent stack overflow
                            json nodes_array = json::array();
                            const int MAX_TREE_DEPTH = 50;  // Prevent stack overflow on deeply nested trees
                            std::function<void(const TreeNode&, json&, int)> convert_node;
                            convert_node = [&](const TreeNode& node, json& node_json, int depth) {
                                if (depth > MAX_TREE_DEPTH) {
                                    spdlog::warn("Tree node conversion reached max depth {}, stopping", MAX_TREE_DEPTH);
                                    return;
                                }

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
                                        convert_node(child, child_json, depth + 1);
                                        children_array.push_back(child_json);
                                    }
                                    node_json["children"] = children_array;
                                }
                            };

                            for (const auto& node : tree_data.nodes) {
                                json node_json;
                                convert_node(node, node_json, 0);  // Start at depth 0
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
                const auto content = elem->get_abap_editor_content(constants::MAX_REQUESTED_TABLE_ROWS);
                metadata["source_total_lines"] = content.total_lines;
                metadata["source_lines_read"] = content.lines_read;
                metadata["source_truncated"] = content.truncated;
                if (!content.text.empty()) {
                    metadata["text_content"] = redact_sensitive_abap_source(content.text);
                    spdlog::debug("Extracted AbapEditor text content ({} chars)", content.text.length());
                } else {
                    metadata["text_content"] = nullptr;  // Empty editor
                    spdlog::debug("AbapEditor is empty");
                }
            } catch (const std::exception& e) {
                spdlog::warn("Failed to extract AbapEditor text: {}", e.what());
                metadata["text_content"] = nullptr;
            }
        }

        // Process children for container elements only
        // Skip child extraction for leaf elements and GuiUserArea
        bool skip_children = (type == "GuiUserArea");
        bool is_container = (!container_type.empty() || type == "GuiContainerShell" ||
                             type == "GuiCustomControl" || type == "GuiSplitterContainer" ||
                             type == "GuiContainerCtrl" || type == "GuiSplitterShell" ||
                             type == "GuiDockShell" || type == "GuiTabStrip" ||
                             type == "GuiToolbar" || type == "GuiMenubar");

        if (depth < constants::MAX_ELEMENT_DEPTH && !skip_children && is_container) {
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
                                if (force_enumerate && child_count == 0) {
                                    spdlog::debug("Force enumerate: Reached end at index {}", i);
                                    break;
                                }
                                continue;
                            }

                            // Directly record child ID (avoiding duplicate recursive extraction
                            // since all elements are traversed at top-level)
                            std::string child_id = child_ptr->get_id();
                            if (!child_id.empty()) {
                                children.push_back(child_id);
                            }
                        } catch (const SapGuiException& e) {
                            if (force_enumerate && child_count == 0) break;
                            spdlog::debug("extract_element_metadata: SapGuiException processing child {}: {}", i, e.what());
                        } catch (const ComException& e) {
                            if (force_enumerate && child_count == 0) break;
                            spdlog::debug("extract_element_metadata: ComException processing child {}: {}", i, e.what());
                        } catch (const std::exception& e) {
                            if (force_enumerate && child_count == 0) break;
                            spdlog::debug("extract_element_metadata: Exception processing child {}: {}", i, e.what());
                        }
                    }
                } catch (const std::exception& e) {
                    spdlog::warn("extract_element_metadata: Failed to get children collection: {}", e.what());
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

