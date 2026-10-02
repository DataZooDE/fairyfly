#include "include/screen_reader.h"
#include "include/action_status.h"
#include "include/field_fill_info.h"
#include "include/element_metadata_extractor.h"
#include "include/screen_element_collector.h"
#include "include/table_data_extractor.h"
#include "include/sensitive_data.h"
#include "include/constants.h"
#include "include/trace.h"
#include <spdlog/spdlog.h>
#include <chrono>
#include <map>
#include <set>
#include <unordered_set>
#include <thread>
#include <algorithm>
#include <cctype>

namespace fairyfly {
namespace sap {

using utils::TraceGuard;

namespace {
std::string lower_ascii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}
}

bool screen_candidate_matches(const ScreenFindOptions& query,
                              const std::string& id,
                              const std::string& name,
                              const std::string& type) {
    return (query.id_contains.empty() || id.find(query.id_contains) != std::string::npos) &&
           (query.name_contains.empty() ||
            lower_ascii(name).find(lower_ascii(query.name_contains)) != std::string::npos) &&
           (query.type.empty() || type == query.type);
}

TabSelectionSnapshot TabSelectionSnapshot::capture(
    const json& tabs,
    const std::function<std::string(const std::string&)>& selected_for_strip) {
    TabSelectionSnapshot snapshot;
    for (const auto& tab : tabs) {
        if (tab.value("type", "") != "GuiTabStrip") continue;
        const auto strip_id = tab.value("id", "");
        if (strip_id.empty()) continue;
        const auto selected_id = selected_for_strip(strip_id);
        if (!selected_id.empty()) snapshot.selected_ids.push_back(selected_id);
    }
    return snapshot;
}

bool TabSelectionSnapshot::restore(
    const std::function<void(const std::string&)>& select_tab) const {
    try {
        for (auto it = selected_ids.rbegin(); it != selected_ids.rend(); ++it)
            select_tab(*it);
        return true;
    } catch (const std::exception&) {
        return false;
    }
}

struct ScreenSearchContext {
    const ScreenFindOptions& query;
    json matches = json::array();
    std::unordered_set<std::string> seen;
    size_t scanned = 0;
    bool stopped = false;
    bool scan_limit_reached = false;
    bool match_limit_reached = false;

    void consider(const ComGuiElementPtr& element, const std::string& id,
                  const std::string& type) {
        if (stopped || !seen.insert(id).second) return;
        if (scanned >= 500) {
            stopped = true;
            scan_limit_reached = true;
            return;
        }
        ++scanned;
        std::string name;
        if (!query.name_contains.empty()) {
            try { name = element->get_name(); } catch (const std::exception&) {}
        }
        if (!screen_candidate_matches(query, id, name, type)) return;
        if (name.empty()) {
            try { name = element->get_name(); } catch (const std::exception&) {}
        }
        json match = {{"id", id}, {"type", type}, {"name", name}};
        if (type == "GuiShell") {
            try { match["subtype"] = element->get_subtype(); } catch (const std::exception&) {}
        }
        const bool has_simple_text = find_type_has_simple_text(type);
        if (has_simple_text) {
            try { match["text"] = element->get_text_for_direct_read(); }
            catch (const std::exception&) { match["text_available"] = false; }
        }
        if (type == "GuiCheckBox" || type == "GuiRadioButton") {
            try { match["selected"] = element->get_property_bool(L"Selected"); }
            catch (const std::exception&) { /* State unavailable. */ }
        }
        if (type_shows_tooltip(type)) {
            // Icon-only buttons have no text: the tooltip is how `screen read only=buttons` names them too.
            std::string tooltip;
            try { tooltip = element->get_tooltip(); } catch (const std::exception&) {}
            attach_tooltip_fields(match, type, tooltip, std::string());
        }
        matches.push_back(std::move(match));
        if (matches.size() >= static_cast<size_t>(query.limit)) {
            stopped = true;
            match_limit_reached = true;
        }
    }
};

ScreenReader::ScreenReader(ComGuiSessionPtr session) : session_(session) {
}

bool ScreenReader::is_tabular_userarea(
    const std::vector<std::tuple<int, int, std::string>>& cells) {
    std::map<int, std::set<int>> occupied;
    for (const auto& [col, row, text] : cells) {
        const auto first = text.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) continue;
        const auto last = text.find_last_not_of(" \t\r\n");
        const auto trimmed = text.substr(first, last - first + 1);
        if (trimmed.find_first_not_of("-=+") == std::string::npos) continue;
        occupied[row].insert(col);
    }
    if (occupied.size() < 2) return false;

    // Match the extraction path's header choice: the row with most text.
    auto header = occupied.end();
    for (auto it = occupied.begin(); it != occupied.end(); ++it) {
        if (header == occupied.end() || it->second.size() > header->second.size())
            header = it;
    }
    if (header->second.size() < 2) return false;

    for (auto it = std::next(header); it != occupied.end(); ++it) {
        int aligned = 0;
        for (int col : it->second)
            if (header->second.count(col)) ++aligned;
        if (aligned >= 2) return true;
    }
    return false;
}

void ScreenReader::flatten_and_group_elements(const json& elements, json& grouped) {
    for (const auto& elem : elements) {
        std::string type = elem.value("type", "");
        std::string container = elem.value("container_type", "");
        std::string elem_id = elem.value("id", "unknown");
        spdlog::debug("flatten_and_group_elements: Processing element type={} id={}", type, elem_id);

        // Group by container type or element type
        if (container == "toolbar" || type == "GuiToolbar" || type == "GuiMenubar") {
            grouped["toolbar"].push_back(elem);
        } else if (type == "GuiShell") {
            std::string subtype = elem.value("subtype", "");
            if (subtype == "Toolbar") {
                grouped["toolbar"].push_back(elem);
            } else if (subtype == "GridView" ||
                       (elem.contains("table_data") && !elem["table_data"].is_null())) {
                grouped["tables"].push_back(elem);
            } else {
                grouped["other"].push_back(elem);
            }
        } else if (type == "GuiButton") {
            // Split buttons: toolbar buttons vs inline buttons
            std::string id = elem.value("id", "");

            // Classify as toolbar button if:
            // 1. In /tbar/ path (standard toolbar)
            // 2. In /titl/ path (title bar buttons)
            // 3. In shell container (shell/btn[X] pattern)
            // 4. NOT in /usr/ (those are inline form buttons)

            bool is_toolbar_button = false;
            if (id.find("/usr/") == std::string::npos) {  // Not in user area
                if (id.find("/tbar[") != std::string::npos ||   // Standard toolbar (e.g., /tbar[0]/btn[1])
                    id.find("/titl") != std::string::npos ||    // Title bar
                    id.find("/shell/btn") != std::string::npos) { // Shell container
                    is_toolbar_button = true;
                }
            }

            if (is_toolbar_button) {
                spdlog::debug("Classifying as toolbar button: {}", id);
                grouped["buttons"].push_back(elem);  // Toolbar buttons
            } else {
                spdlog::debug("Classifying as inline button: {}", id);
                grouped["inline_buttons"].push_back(elem);  // Inline buttons
            }
        } else if (type == "GuiTextField" || type == "GuiCTextField" ||
                   type == "GuiPasswordField" || type == "GuiComboBox" ||
                   type == "GuiCheckBox" || type == "GuiRadioButton") {
            grouped["form_fields"].push_back(elem);
        } else if (type == "GuiLabel") {
            // Labels go to form_fields if they're next to input fields
            grouped["form_fields"].push_back(elem);
        } else if (container == "table" || container == "grid" ||
                   type == "GuiTableControl" || type == "GuiGridView" ||
                   (type == "GuiShell" && elem.value("subtype", "") == "GridView") ||
                   (elem.contains("table_data") && !elem["table_data"].is_null())) {
            grouped["tables"].push_back(elem);
        } else if (container == "tabs" || type == "GuiTabStrip" || type == "GuiTab") {
            grouped["tabs"].push_back(elem);
        } else {
            grouped["other"].push_back(elem);
        }

        // Recursively process children
        // NOTE: After optimization, children are stored as ID strings, not full objects
        // Full child data is in the top-level elements array, so no recursion needed
        if (elem.contains("children") && elem["children"].is_array() && !elem["children"].empty()) {
            // Check if children are objects (old format) or strings (new optimized format)
            if (elem["children"][0].is_object()) {
                // Old format: children contain full objects, recurse
                size_t child_count = elem["children"].size();
                spdlog::debug("Processing {} children of element type={} (object format)", child_count, type);
                flatten_and_group_elements(elem["children"], grouped);
            } else {
                // New format: children are just ID strings, skip recursion
                // (full child data is already in the top-level elements array)
                spdlog::debug("Skipping recursion for {} children of type={} (ID-only format)",
                             elem["children"].size(), type);
            }
        }
    }
}

json ScreenReader::group_elements_by_container(const json& elements) {
    json grouped;
    grouped["toolbar"] = json::array();
    grouped["buttons"] = json::array();
    grouped["inline_buttons"] = json::array();
    grouped["form_fields"] = json::array();
    grouped["tables"] = json::array();
    grouped["tabs"] = json::array();
    grouped["other"] = json::array();

    flatten_and_group_elements(elements, grouped);

    // Deduplicate all groups by element ID
    auto deduplicate = [](json& arr) {
        std::unordered_set<std::string> seen_ids;
        json deduped = json::array();
        for (const auto& elem : arr) {
            std::string id = elem.value("id", "");
            if (!id.empty() && seen_ids.insert(id).second) {
                deduped.push_back(elem);
            }
        }
        arr = deduped;
    };

    deduplicate(grouped["toolbar"]);
    deduplicate(grouped["buttons"]);
    deduplicate(grouped["inline_buttons"]);
    deduplicate(grouped["form_fields"]);
    deduplicate(grouped["tables"]);
    deduplicate(grouped["tabs"]);
    deduplicate(grouped["other"]);

    // Remove empty groups
    if (grouped["toolbar"].empty()) grouped.erase("toolbar");
    if (grouped["buttons"].empty()) grouped.erase("buttons");
    if (grouped["inline_buttons"].empty()) grouped.erase("inline_buttons");
    if (grouped["form_fields"].empty()) grouped.erase("form_fields");
    if (grouped["tables"].empty()) grouped.erase("tables");
    if (grouped["tabs"].empty()) grouped.erase("tabs");
    if (grouped["other"].empty()) grouped.erase("other");

    return grouped;
}

json ScreenReader::extract_tree_data_immediately(ComGuiElementPtr element, const std::string& elem_id) {
    json tree_element;
    tree_element["id"] = elem_id;
    tree_element["type"] = "GuiShell";

    try {
        // Get subtype to confirm it's a tree
        std::string subtype = element->get_subtype();
        tree_element["subtype"] = subtype;

        // Only extract if it's actually a tree
        if (subtype != "Tree" && subtype != "TableTreeControl") {
            spdlog::debug("GuiShell element {} is not a tree (subtype={}), skipping tree extraction", elem_id, subtype);
            tree_element["tree_nodes"] = json::array();
            return tree_element;
        }

        // Extract tree nodes using safe tree API
        spdlog::debug("Extracting tree data from {} using tree API...", elem_id);

        // Get all node keys (safe API, no Children traversal)
        auto node_keys = element->get_all_node_keys();
        if (node_keys.empty()) {
            spdlog::warn("Tree {} extraction returned 0 nodes - tree may be empty or API call failed", elem_id);
            tree_element["extraction_warning"] = "No nodes returned by GetAllNodeKeys() - tree may be empty";
        } else {
            spdlog::debug("Found {} nodes in tree {}", node_keys.size(), elem_id);
        }

        // Limit to 1000 nodes to prevent performance issues
        const size_t MAX_NODES = 1000;
        if (node_keys.size() > MAX_NODES) {
            spdlog::warn("Tree {} has {} nodes, limiting to {} for performance",
                        elem_id, node_keys.size(), MAX_NODES);
            node_keys.resize(MAX_NODES);
        }

        // Get column names for extracting column data
        std::vector<std::string> columns;
        try {
            columns = element->get_column_order();
            spdlog::debug("Tree {} has {} columns", elem_id, columns.size());
        } catch (const std::exception& e) {
            spdlog::debug("Could not get column order for {}: {}", elem_id, e.what());
        }

        // Extract each node with robust per-property error handling
        json nodes_array = json::array();
        int failed_nodes = 0;
        std::vector<std::string> tree_column_names;
        bool tree_column_names_loaded = false;

        for (size_t i = 0; i < node_keys.size(); ++i) {
            const auto& key = node_keys[i];
            try {
                spdlog::debug("Extracting node {}/{}: key='{}'", i+1, node_keys.size(), key);
                auto node_start = std::chrono::high_resolution_clock::now();

                json node;
                node["key"] = key;

                // Extract node text with individual error handling
                try {
                    spdlog::debug("  -> Getting node text for key '{}'", key);
                    std::string node_text = element->get_node_text_by_key(key);
                    if (node_text.empty() && !tree_column_names_loaded) {
                        tree_column_names = element->get_tree_column_names();
                        tree_column_names_loaded = true;
                    }
                    node["text"] = recover_tree_node_text(
                        node_text, tree_column_names,
                        [&](const std::string& name) { return element->get_item_text(key, name); });
                    spdlog::debug("  -> Got node text for key '{}'", key);
                } catch (const std::exception& e) {
                    node["text"] = "[extraction_failed]";
                    spdlog::warn("Failed to extract text for node key '{}' in tree {}: {}", key, elem_id, e.what());
                }

                // Extract node path with individual error handling
                try {
                    spdlog::debug("  -> Getting node path for key '{}'", key);
                    node["path"] = element->get_node_path_by_key(key);
                    spdlog::debug("  -> Got node path: '{}'", node["path"].get<std::string>());
                } catch (const std::exception& e) {
                    node["path"] = "[extraction_failed]";
                    spdlog::warn("Failed to extract path for node key '{}' in tree {}: {}", key, elem_id, e.what());
                }

                // Extract column values using GetItemText
                if (!columns.empty()) {
                    spdlog::debug("  -> Extracting {} columns for key '{}'", columns.size(), key);
                    node["available_columns"] = columns;

                    json column_values = json::array();
                    auto col_start = std::chrono::high_resolution_clock::now();

                    for (const json& col_name_json : columns) {
                        try {
                            std::string col_name_str = col_name_json.is_string() ? col_name_json.get<std::string>() : "";
                            if (col_name_str.empty() || col_name_str == "HierarchyHeader") {
                                // Skip hierarchy column (already have text/path)
                                column_values.push_back("");
                                continue;
                            }

                            std::string col_value = element->get_item_text(key, col_name_str);
                            column_values.push_back(col_value);

                        } catch (const std::exception& e) {
                            spdlog::debug("  -> Failed to get column '{}' for key '{}': {}",
                                        col_name_json.is_string() ? col_name_json.get<std::string>() : "?", key, e.what());
                            column_values.push_back("");  // Empty value on failure
                        }
                    }

                    node["column_values"] = column_values;

                    auto col_duration = std::chrono::high_resolution_clock::now() - col_start;
                    auto col_ms = std::chrono::duration_cast<std::chrono::milliseconds>(col_duration).count();
                    if (col_ms > 500) {
                        spdlog::warn("  -> Column extraction for key '{}' took {}ms", key, col_ms);
                    }
                }

                nodes_array.push_back(node);

                // Log if this node took unusually long
                auto node_duration = std::chrono::high_resolution_clock::now() - node_start;
                auto node_ms = std::chrono::duration_cast<std::chrono::milliseconds>(node_duration).count();
                if (node_ms > 1000) {
                    spdlog::warn("Node '{}' extraction took {}ms", key, node_ms);
                }

            } catch (const std::exception& e) {
                // Entire node extraction failed - add error marker node
                spdlog::warn("Failed to extract node '{}' from tree {}: {}", key, elem_id, e.what());
                failed_nodes++;

                json error_node;
                error_node["key"] = key;
                error_node["text"] = "[extraction_error]";
                error_node["path"] = "[extraction_error]";
                error_node["error"] = e.what();
                nodes_array.push_back(error_node);
            }
        }

        if (failed_nodes > 0) {
            spdlog::warn("Tree {}: {} out of {} nodes had extraction errors",
                        elem_id, failed_nodes, node_keys.size());
        }

        tree_element["tree_nodes"] = nodes_array;
        tree_element["node_count"] = nodes_array.size();

        spdlog::info("Successfully extracted {} nodes from tree {}", nodes_array.size(), elem_id);

    } catch (const std::exception& e) {
        spdlog::error("Failed to extract tree data from {}: {}", elem_id, e.what());
        tree_element["tree_nodes"] = json::array();
        tree_element["error"] = e.what();
    }

    return tree_element;
}

json ScreenReader::extract_grid_data_immediately(ComGuiElementPtr element, const std::string& elem_id) {
    json grid_element;
    grid_element["id"] = elem_id;

    try {
        std::string type = element->get_type();
        grid_element["type"] = type;

        if (type == "GuiShell") {
            try {
                std::string subtype = element->get_subtype();
                if (!subtype.empty()) {
                    grid_element["subtype"] = subtype;
                }
            } catch (const ComException&) {
                // Subtype may not be available on all GuiShell elements
            } catch (const std::exception&) {
                // Subtype may not be available on all GuiShell elements
            }
        }

        spdlog::debug("Extracting grid data from {} (type={})", elem_id, type);

        // Extract grid data using TableDataExtractor
        TableExtractionOptions options;
        // Grids an `only` selector drops anyway need no cell reads (they cost the most COM calls).
        options.max_rows = grid_rows_needed_ ? max_rows_ : 0;
        options.row_offset = row_offset_;
        options.include_headers = true;

        TableDataExtractor extractor(options);
        TableData grid_data;
        if (type == "GuiTableControl") {
            grid_data = extractor.extract_table_data(element);
        } else {
            grid_data = extractor.extract_grid_data(element);
        }


        if (!grid_data.rows.empty() || !grid_data.columns.empty()) {
            json table_json;
            table_json["columns"] = grid_data.columns;
            table_json["rows"] = grid_data.rows;
            table_json["total_row_count"] = grid_data.total_row_count;
            table_json["visible_row_count"] = grid_data.visible_row_count;
            redact_sensitive_header_rows(table_json);
            annotate_row_window(table_json, grid_data.row_offset, grid_data.empty_rows_trimmed);
            if (type != "GuiTableControl") {
                const auto viewport = extract_grid_viewport_metadata(element);
                table_json.update(viewport);
            }

            grid_element["table_data"] = table_json;
            spdlog::info("Extracted {} rows × {} columns from grid {}",
                        grid_data.rows.size(), grid_data.columns.size(), elem_id);
        } else {
            spdlog::warn("Grid {} extraction returned no data (empty grid)", elem_id);
        }

        if (type == "GuiShell" || type == "GuiGridView") {
            json buttons = json::array();
            const int count = std::min(element->get_grid_toolbar_button_count(), 100);
            for (int i = 0; i < count; ++i) {
                const std::string button_id = element->get_grid_toolbar_button_id(i);
                if (button_id.empty()) continue; // Separator or menu gap.
                buttons.push_back({
                    {"id", elem_id + "/btn_" + button_id},
                    {"type", "GuiButton"},
                    {"name", button_id},
                    {"tooltip", element->get_grid_toolbar_button_tooltip(i)},
                    {"capabilities", json::array({"clickable"})}
                });
            }
            if (!buttons.empty()) {
                grid_element["toolbar_buttons"] = buttons;
                grid_element["button_count"] = buttons.size();
            }
        }

    } catch (const std::exception& e) {
        spdlog::error("Failed to extract grid data from {}: {}", elem_id, e.what());
        grid_element["extraction_error"] = e.what();
    }

    return grid_element;
}

void ScreenReader::limit_userarea_table_rows(json& table, int max_rows, int offset) {
    auto& rows = table.at("rows");
    const auto skip = std::min(rows.size(), static_cast<size_t>(std::max(0, offset)));
    if (skip > 0) rows.erase(rows.begin(), rows.begin() + static_cast<json::difference_type>(skip));
    const auto limit = static_cast<size_t>(std::max(0, max_rows));
    if (rows.size() > limit) {
        rows.erase(rows.begin() + static_cast<json::difference_type>(limit), rows.end());
    }
    annotate_row_window(table, static_cast<int>(skip), 0);
}

json ScreenReader::extract_userarea_grid_data(ComGuiElementPtr element, const std::string& elem_id,
                                              const ScreenElementCollector& collector) {
    json grid_element;
    grid_element["id"] = elem_id;
    grid_element["type"] = "GuiUserArea";
    grid_element["subtype"] = "GridView";
    grid_element["container_type"] = "table";
    grid_element["name"] = "Table";
    grid_element["enabled"] = true;
    grid_element["visible"] = true;
    grid_element["changeable"] = false;
    grid_element["capabilities"] = json::array({"table", "grid"});

    try {
        const auto& cells = collector.get_grid_cells(elem_id);

        if (cells.empty()) {
            spdlog::warn("No grid cells found for GuiUserArea {}", elem_id);
            grid_element["table_data"] = nullptr;
            return grid_element;
        }

        spdlog::debug("Extracting grid data from {} ({} cells)", elem_id, cells.size());

        // Group cells by [row][col] -> text
        std::map<int, std::map<int, std::string>> grid_map;
        for (const auto& [col, row, text] : cells) {
            std::string trimmed = text;
            size_t first = trimmed.find_first_not_of(" \t\r\n");
            if (first != std::string::npos) {
                size_t last = trimmed.find_last_not_of(" \t\r\n");
                trimmed = trimmed.substr(first, last - first + 1);
            } else {
                trimmed = "";
            }
            grid_map[row][col] = trimmed;
        }

        auto is_separator = [](const std::string& s) {
            if (s.empty()) return false;
            for (char ch : s) {
                if (ch != '-' && ch != '=' && ch != '+') return false;
            }
            return true;
        };

        // Header detection: find row with most non-empty non-separator cells
        int header_row = -1;
        size_t max_header_cols = 0;

        for (const auto& [r, cols] : grid_map) {
            size_t non_empty = 0;
            for (const auto& [c, val] : cols) {
                if (!val.empty() && !is_separator(val)) {
                    non_empty++;
                }
            }
            if (non_empty > max_header_cols) {
                max_header_cols = non_empty;
                header_row = r;
            }
        }

        if (header_row == -1 || max_header_cols == 0) {
            header_row = grid_map.begin()->first;
        }

        // Distinct column positions from header row
        std::vector<int> col_positions;
        json header_array = json::array();

        for (const auto& [c, val] : grid_map[header_row]) {
            if (!val.empty() && !is_separator(val)) {
                col_positions.push_back(c);
                header_array.push_back(val);
            }
        }

        // Extract data rows (rows > header_row)
        json data_rows = json::array();
        for (const auto& [r, cols] : grid_map) {
            if (r <= header_row) continue;

            bool has_data = false;
            for (const auto& [c, val] : cols) {
                if (!val.empty() && !is_separator(val)) {
                    has_data = true;
                    break;
                }
            }
            if (!has_data) continue;

            json row_values = json::array();
            for (int c_pos : col_positions) {
                auto it = cols.find(c_pos);
                if (it != cols.end()) {
                    row_values.push_back(it->second);
                } else {
                    row_values.push_back("");
                }
            }
            data_rows.push_back(row_values);
        }

        json table_json;
        table_json["columns"] = header_array;
        table_json["rows"] = data_rows;
        table_json["total_row_count"] = data_rows.size();
        table_json["visible_row_count"] = data_rows.size();
        table_json["column_count"] = header_array.size();
        redact_sensitive_header_rows(table_json);
        limit_userarea_table_rows(table_json, max_rows_, row_offset_);

        grid_element["table_data"] = table_json;

        spdlog::info("Extracted {} rows × {} columns from GuiUserArea grid {}",
                     data_rows.size(), header_array.size(), elem_id);

    } catch (const std::exception& e) {
        spdlog::error("Failed to extract UserArea grid data from {}: {}", elem_id, e.what());
        grid_element["table_data"] = nullptr;
        grid_element["extraction_error"] = e.what();
    }

    return grid_element;
}

namespace {
// Positioned label cell (/lbl[col,row]) text with a single Text read. The type is implied by
// the id prefix, so identity is primed instead of read. Redaction matches
// ComGuiElement::get_text for labels (structured text carrying sensitive names).
std::string read_positioned_label_text(const ComGuiElementPtr& label, const std::string& id) {
    label->prime_identity(id, "GuiLabel");
    const std::string text = label->get_string_property(L"Text");
    const bool structured = text.find('=') != std::string::npos ||
                            text.find(':') != std::string::npos;
    if (structured && contains_sensitive_data_name(text)) return redact_sensitive_response_text(text);
    return text;
}
} // namespace

std::vector<std::string> id_probe_candidates(const std::string& type,
                                             const std::string& elem_id,
                                             int children_found,
                                             bool probe_all) {
    static const std::set<std::string> shell_hosts = {
        "GuiCustomControl", "GuiContainerShell", "GuiSplitterShell",
        "GuiSplitterContainer", "GuiDockShell", "GuiContainerCtrl"};
    static const std::set<std::string> subscreen_hosts = {
        "GuiSimpleContainer", "GuiScrollContainer", "GuiSubScreen", "GuiTab"};
    // Deliberately never probed (Children is complete and probing only produced misses):
    // GuiToolbar, GuiTitlebar, GuiMenubar, GuiTabStrip, GuiBox, GuiStatusbar, GuiUserArea,
    // and every leaf/unknown type not listed above.
    std::vector<std::string> ids;
    const bool want_shell = probe_all || shell_hosts.count(type);
    const bool want_subscreen = probe_all || (subscreen_hosts.count(type) && children_found <= 0);
    if (want_shell) {
        ids.push_back(elem_id + "/shell");
        for (int i = 0; i < constants::MAX_SHELLCONT_CHILDREN; ++i)
            ids.push_back(elem_id + "/shell[" + std::to_string(i) + "]");
        for (int i = 0; i < constants::MAX_SHELLCONT_CHILDREN; ++i)
            ids.push_back(elem_id + "/shellcont[" + std::to_string(i) + "]");
        ids.push_back(elem_id + "/shellcont");
    }
    if (want_subscreen) {
        // Subscreens whose dynamically generated fields (SE16 preselection, SEGW) are
        // reachable by FindById only.
        for (int i = 0; i < constants::MAX_SUB_CONTAINERS; ++i)
            ids.push_back(elem_id + "/sub[" + std::to_string(i) + "]");
        for (const char* name : {"IMAGE_CONTAINER", "GRID_CONTAINER", "TREE_CONTAINER",
                                 "CUSTOM_CONTROL", "CC_CONTAINER"})
            ids.push_back(elem_id + "/cntl" + name);
        for (int i = 0; i < constants::MAX_SHELLCONT_CHILDREN; ++i)
            ids.push_back(elem_id + "/cntl[" + std::to_string(i) + "]");
        for (const char* name : {"SCR_PRESEL", "SCR_SEL", "AREA", "MAIN", "HEADER", "DETAIL"})
            ids.push_back(elem_id + "/ssub" + name);
    }
    return ids;
}

std::vector<std::string> id_probe_candidates(const std::string& type,
                                             const std::string& elem_id,
                                             int children_found,
                                             const ScreenElementCollector& collector,
                                             bool probe_all) {
    auto ids = id_probe_candidates(type, elem_id, children_found, probe_all);
    ids.erase(std::remove_if(ids.begin(), ids.end(),
                             [&](const std::string& id) { return collector.contains(id); }),
              ids.end());
    return ids;
}

bool probe_all_from_environment() {
    char* buffer = nullptr;
    size_t size = 0;
    bool enabled = false;
    if (_dupenv_s(&buffer, &size, "FAIRYFLY_PROBE_ALL") == 0 && buffer != nullptr) {
        enabled = std::string(buffer) == "1";
        free(buffer);
    }
    return enabled;
}

void ScreenReader::traverse_element_tree(
    ComGuiElementPtr element,
    ScreenElementCollector& collector,
    int depth,
    bool skip_trees,
    ScreenSearchContext* search) {
    const int MAX_DEPTH = 15;  // Limit depth (SEGW needs depth 10+)

    if (!element || !session_ || depth >= MAX_DEPTH || (search && search->stopped)) {
        return;
    }

    // CRITICAL: Wrap property access in try-catch to prevent segfaults from problematic elements (esp. tree controls)
    std::string type;
    std::string elem_id;

    try {
        type = element->get_type();
    } catch (const std::exception& e) {
        spdlog::debug("{}[depth={}] Skipping element - get_type() failed: {}",
                     std::string(depth * 2, ' '), depth, e.what());
        return;  // Skip this element entirely - can't safely process it
    }

    // Early tree detection - skip BEFORE calling get_id() which might also crash
    if (skip_trees && type == "GuiTree") {
        spdlog::debug("{}[depth={}] Skipping GuiTree element (--skip-trees enabled)",
                     std::string(depth * 2, ' '), depth);
        return;
    }

    try {
        elem_id = element->get_id();
    } catch (const std::exception& e) {
        spdlog::debug("{}[depth={}] Skipping element type={} - get_id() failed: {}",
                     std::string(depth * 2, ' '), depth, type, e.what());
        return;  // Skip this element entirely
    }

    if (search) {
        search->consider(element, elem_id, type);
        if (search->stopped) return;
    }

    // Check GuiShell controls - distinguish between GridView and Tree controls
    if (type == "GuiShell") {
        std::string subtype = "";
        try {
            subtype = element->get_string_property(L"SubType");
        } catch (...) {
            // Some GuiShell instances might not expose SubType
        }

        const auto extraction_kind = classify_shell_extraction(subtype);
        if (extraction_kind == ShellExtractionKind::Grid) {
            spdlog::debug("Found GuiShell GridView {} at depth {} - collecting ID for Phase 2 grid extraction", elem_id, depth);
            collector.add_grid_id(elem_id);
            return;
        }

        if (extraction_kind == ShellExtractionKind::Metadata) {
            collector.add(element);
            return;
        }

        if (skip_trees) {
            // When --skip-trees is enabled, completely skip GuiShell elements
            spdlog::debug("{}[depth={}] Skipping GuiShell element {} (--skip-trees enabled)",
                         std::string(depth * 2, ' '), depth, elem_id);
            return;
        } else {
            // PHASE 1: Collect tree ID for later extraction with fresh COM pointer
            // Don't extract now - the traversal pointer may become stale
            // Will extract in Phase 2 after traversal completes using session->find_element_by_id()
            spdlog::debug("Found GuiShell {} (subtype='{}') at depth {} - collecting ID for Phase 2 extraction", elem_id, subtype, depth);
            collector.add_tree_id(elem_id);

            // Don't traverse children or store COM pointer - will extract later
            return;
        }
    }

    // Check for GuiGridView / GuiTableControl - collect for Phase 2 extraction
    if (type == "GuiGridView" || type == "GuiTableControl") {
        spdlog::debug("Found grid {} (type={}) at depth {} - collecting ID for Phase 2 extraction",
                     elem_id, type, depth);
        collector.add_grid_id(elem_id);
        // Don't traverse children - grids are leaf elements with no meaningful child structure
        return;
    }

    try {
        // CRITICAL: Do NOT add the element to collector if it might cause problems later
        // Even if we can get type/id now, subsequent property access might crash
        // This is a defense-in-depth approach: skip during discovery AND during extraction

        // Add current element to collector (with O(1) deduplication)
        // If already seen, skip processing to avoid duplicate work
        if (!collector.add(element)) {
            return;  // Already processed this element
        }
        collector.set_known_type(elem_id, type);

        spdlog::debug("{}[depth={}] {} ({})",
                     std::string(depth * 2, ' '), depth, elem_id, type);

        // Known leaf types that should never be traversed as containers
        // GuiMenu is treated as a leaf so top-level menus are captured, but 100+ deep nested submenus are skipped
        bool is_known_leaf = (type == "GuiButton" || type == "GuiTextField" || type == "GuiCTextField" ||
                              type == "GuiPasswordField" || type == "GuiRadioButton" || type == "GuiCheckBox" ||
                              type == "GuiLabel" || type == "GuiStatusbar" || type == "GuiMenu");

        if (is_known_leaf) {
            return;
        }

        // Introspect child count directly from element
        int child_count = 0;
        try {
            child_count = element->get_child_count();
        } catch (...) {
            child_count = 0;
        }

        // Container types that have nested structure or report child_count > 0
        bool is_container = (child_count > 0 ||
                           type == "GuiContainerShell" ||
                           type == "GuiSplitterShell" ||
                           type == "GuiSimpleContainer" ||   // Subscreen containers (SE16 selection screens)
                           type == "GuiScrollContainer" ||   // Scrollable subscreen containers
                           type == "GuiUserArea" ||
                           type == "GuiCustomControl" ||
                           type == "GuiToolbar" ||           // Toolbar contains buttons
                           type == "GuiToolbarControl" ||    // Toolbar control
                           type == "GuiTitlebar" ||          // Title bar may contain buttons
                           type == "GuiTabStrip" ||          // Tab strip contains tabs
                           type == "GuiTab" ||               // Tab contains controls
                           type == "GuiSubScreen" ||         // Subscreen contains controls
                           type == "GuiBox");                // Group box contains controls

        if (!is_container) {
            return;  // Leaf element, no children to traverse
        }

        // Special handling for GuiUserArea: separate tabular grid cells (/lbl[col,row]) from form controls
        // to avoid polluting the element collector with hundreds of individual cell labels.
        if (type == "GuiUserArea") {
            spdlog::debug("{}Enumerating GuiUserArea children for {}", std::string(depth * 2, ' '), elem_id);
            try {
                auto children_collection = element->children();
                int total_items = std::max(child_count, 500);
                struct CellInfo {
                    ComGuiElementPtr ptr;
                    int col;
                    int row;
                    std::string text;
                };
                std::vector<CellInfo> grid_cells;

                // Handles one child; returns false to stop the walk (first error, like before).
                auto handle_child = [&](const ComGuiElementPtr& child_ptr) -> bool {
                    try {
                        std::string child_id = child_ptr->get_id();

                        if (search) {
                            traverse_element_tree(child_ptr, collector, depth + 1,
                                                  skip_trees, search);
                            return true;
                        }

                        // Check if child is a grid cell label: /lbl[col,row]
                        size_t bracket_pos = child_id.find("/lbl[");
                        if (bracket_pos != std::string::npos) {
                            size_t comma_pos = child_id.find(",", bracket_pos);
                            size_t close_bracket = child_id.find("]", comma_pos);
                            if (comma_pos != std::string::npos && close_bracket != std::string::npos) {
                                try {
                                    int col = std::stoi(child_id.substr(bracket_pos + 5, comma_pos - bracket_pos - 5));
                                    int row = std::stoi(child_id.substr(comma_pos + 1, close_bracket - comma_pos - 1));
                                    std::string text = read_positioned_label_text(child_ptr, child_id);
                                    grid_cells.push_back({child_ptr, col, row, text});
                                    return true; // Captured as grid cell, do not recurse or add to collector
                                } catch (...) {}
                            }
                        }

                        // Non-label child: traverse recursively (e.g. buttons, subscreens, input fields)
                        traverse_element_tree(child_ptr, collector, depth + 1, skip_trees, search);
                        return true;
                    } catch (const std::exception&) {
                        return false; // Stop on first error
                    }
                };

                // Fast path: one IEnumVARIANT walk (1 COM call per child) instead of item(i)
                // (~4-5 round trips per child). Same order, same limit, same stop conditions.
                int enumerated = 0;
                bool enumerated_ok = children_collection.for_each(
                    [&](const ComGuiElementPtr& child_ptr) {
                        if (enumerated >= total_items) return false;
                        if (search && search->stopped) return false;
                        ++enumerated;
                        return handle_child(child_ptr);
                    });
                if (search && search->stopped) return;

                if (!enumerated_ok || enumerated == 0) {
                    for (int i = 0; i < total_items; ++i) {
                        if (search && search->stopped) return;
                        try {
                            ComGuiElementPtr child_ptr = children_collection.item(i);
                            if (!child_ptr) {
                                break; // Stop at first null
                            }
                            if (!handle_child(child_ptr)) break;
                        } catch (const std::exception&) {
                            break; // Stop on first error
                        }
                    }
                }

                // Evaluate whether collected grid cells form a tabular grid
                if (!grid_cells.empty()) {
                    std::vector<std::tuple<int, int, std::string>> cell_tuples;
                    cell_tuples.reserve(grid_cells.size());
                    for (const auto& cell : grid_cells)
                        cell_tuples.emplace_back(cell.col, cell.row, cell.text);

                    const bool is_table = is_tabular_userarea(cell_tuples);

                    if (is_table) {
                        spdlog::info("GuiUserArea {} contains tabular grid ({} cells) - registering for table extraction",
                                     elem_id, grid_cells.size());
                        collector.add_grid_id(elem_id);
                        collector.set_grid_cells(elem_id, cell_tuples);
                    } else {
                        // Not a tabular grid: traverse each cell as a normal form label
                        for (const auto& cell : grid_cells) {
                            traverse_element_tree(cell.ptr, collector, depth + 1, skip_trees);
                        }
                    }
                }
            } catch (const std::exception& e) {
                spdlog::debug("{}GuiUserArea children enumeration failed: {}", std::string(depth * 2, ' '), e.what());
            }

            // --probe-all keeps going into the exhaustive FindById probing below.
            if (search && !probe_all_ && !probe_all_from_environment()) return;

            // GuiUserArea does not contain /shell or /shellcont container children
            if (!probe_all_ && !probe_all_from_environment()) return;
        }

        // Try Children collection first, but track if it actually works
        try {
            if (child_count > 0) {
                // Fast path: one enumeration for all children (get_child(i) re-enumerates per index).
                int visited = 0;
                bool enumerated_ok = false;
                try {
                    enumerated_ok = element->children().for_each(
                        [&](const ComGuiElementPtr& child) {
                            if (visited >= child_count) return false;
                            if (search && search->stopped) return false;
                            ++visited;
                            try {
                                traverse_element_tree(child, collector, depth + 1, skip_trees, search);
                            } catch (const std::exception&) {
                                // Child access failed, will fall back to ID-based
                            }
                            return true;
                        });
                } catch (const std::exception&) {
                    // Children collection not available
                }
                if (search && search->stopped) return;

                if (!enumerated_ok || visited == 0) {
                    for (int i = 0; i < child_count; ++i) {
                        if (search && search->stopped) return;
                        try {
                            auto child = element->get_child(i);
                            if (child) {
                                traverse_element_tree(child, collector, depth + 1, skip_trees, search);
                            }
                        } catch (const SapGuiException&) {
                            // Child access failed, will fall back to ID-based
                        } catch (const ComException&) {
                            // Child access failed, will fall back to ID-based
                        } catch (const std::exception&) {
                            // Child access failed, will fall back to ID-based
                        }
                    }
                }
                // Don't return here - continue with ID-based discovery
                // SAP GUI can have elements accessible via FindById but not Children collection
            }
        } catch (const std::exception&) {
            // Children collection not available
        }

        if (search && search->stopped) return;

        // ID-based discovery is only a safety net for controls that FindById can reach but
        // Children does not list. Which paths are worth a FindById round trip depends on the
        // control type and on whether Children yielded anything (see id_probe_candidates).
        const auto probes = id_probe_candidates(type, elem_id, child_count, collector,
                                                probe_all_ || probe_all_from_environment());
        if (!probes.empty()) {
            spdlog::debug("{}[depth={}] Probing {} candidate id(s) for {} ({})",
                          std::string(depth * 2, ' '), depth, probes.size(), elem_id, type);
        }
        std::string missed_index_family;
        for (const auto& probe_id : probes) {
            if (search && search->stopped) return;
            const auto bracket = probe_id.find('[');
            const std::string family = bracket == std::string::npos ? std::string() : probe_id.substr(0, bracket);
            // Indexed families (/shell[N], /shellcont[N], /sub[N], /cntl[N]) end at the first miss.
            if (!family.empty() && family == missed_index_family) continue;
            try {
                auto probed = session_->find_element_by_id(probe_id);
                if (probed) {
                    spdlog::debug("Found probed child: {}", probe_id);
                    traverse_element_tree(probed, collector, depth + 1, skip_trees, search);
                }
            } catch (const std::exception& e) {
                spdlog::trace("No probed child {}: {}", probe_id, e.what());
                if (!family.empty()) missed_index_family = family;
            }
        }
    } catch (const SapGuiException& e) {
        spdlog::debug("traverse_element_tree: SapGuiException: {}", e.what());
        // Skip elements that throw exceptions during processing
    } catch (const ComException& e) {
        spdlog::debug("traverse_element_tree: ComException: {}", e.what());
        // Skip elements that throw exceptions during processing
    } catch (const std::exception& e) {
        spdlog::debug("traverse_element_tree: std::exception: {}", e.what());
        // Skip elements that throw exceptions during processing
    }
}

void ScreenReader::extract_collected_trees_and_grids(ScreenElementCollector& collector) {
    // PHASE 2: Extract tree data using fresh COM pointers (matches VBScript pattern)
    // After traversal completes, get each GuiShell by direct ID lookup for stable pointer
    const auto& tree_ids = collector.get_tree_ids();
    const auto& grid_ids = collector.get_grid_ids();

    if (!tree_ids.empty()) {
        spdlog::debug("PHASE 2: Extracting {} tree element(s) with fresh COM pointers", tree_ids.size());

        // CRITICAL: Validate session is still alive before using it
        // Session pointer can become stale if SAP GUI reorganizes connections
        if (!session_->is_alive()) {
            spdlog::error("PHASE 2 ABORTED: Session became invalid between Phase 1 and Phase 2 (connection reorganization detected)");
            spdlog::error("This indicates SAP GUI closed/recreated the connection during traversal");
            // Skip tree extraction but return partial results from Phase 1
            return;
        }

        for (const auto& tree_id : tree_ids) {
            try {
                // Get FRESH pointer by direct ID lookup (like VBScript session.findById())
                auto tree_element = session_->find_element_by_id(tree_id);
                if (!tree_element) {
                    spdlog::warn("Could not find tree element by ID: {}", tree_id);
                    continue;
                }

                // Extract tree data from fresh pointer
                spdlog::debug("Extracting tree data from {} using fresh COM pointer", tree_id);
                json tree_data = extract_tree_data_immediately(tree_element, tree_id);

                // Store extracted data
                collector.add_extracted_data(tree_id, tree_data);

                spdlog::debug("Successfully extracted {} nodes from tree {}",
                           tree_data.value("node_count", 0), tree_id);

            } catch (const ComException& e) {
                spdlog::error("Failed to extract tree {} in Phase 2: COM error: {} (HRESULT: 0x{:08X})",
                             tree_id, e.what(), e.hresult());
                spdlog::error("Possible causes: stale COM pointer, connection lost, or SAP GUI reorganized connections");
                // Continue with other trees even if one fails
            } catch (const std::exception& e) {
                spdlog::error("Failed to extract tree {} in Phase 2: {}", tree_id, e.what());
                // Continue with other trees even if one fails
            }
        }
    }

    // PHASE 2B: Extract grid data using fresh COM pointers (same pattern as trees)
    // GuiGridView and GuiTableControl elements need fresh pointers to avoid stale pointer issues
    if (!grid_ids.empty()) {
        spdlog::debug("PHASE 2B: Extracting {} grid element(s) with fresh COM pointers", grid_ids.size());

        // CRITICAL: Validate session is still alive before using it
        if (!session_->is_alive()) {
            spdlog::error("PHASE 2B ABORTED: Session became invalid (connection reorganization detected)");
            return;
        }

        for (const auto& grid_id : grid_ids) {
            try {
                // Get FRESH pointer by direct ID lookup
                auto grid_element = session_->find_element_by_id(grid_id);
                if (!grid_element) {
                    spdlog::warn("Could not find grid element by ID: {}", grid_id);
                    continue;
                }

                // Extract grid data from fresh pointer
                spdlog::debug("Extracting grid data from {} using fresh COM pointer", grid_id);

                // Determine grid type and use appropriate extraction method
                std::string grid_type = grid_element->get_type();
                json grid_data;

                if (grid_type == "GuiUserArea") {
                    // Extract grid from positioned labels (lbl[row,col])
                    grid_data = extract_userarea_grid_data(grid_element, grid_id, collector);
                } else if (grid_type == "GuiGridView" || grid_type == "GuiTableControl" || grid_type == "GuiShell") {
                    // Extract grid using SAP GUI grid APIs
                    grid_data = extract_grid_data_immediately(grid_element, grid_id);
                } else {
                    spdlog::warn("Unknown grid type {} for element {}, skipping", grid_type, grid_id);
                    continue;
                }

                // Store extracted data
                collector.add_extracted_data(grid_id, grid_data);

                // Safely extract row count - grid_data["table_data"] might be null
                int row_count = 0;
                if (grid_data.contains("table_data") && grid_data["table_data"].is_object()) {
                    auto rows = grid_data["table_data"].value("rows", json::array());
                    row_count = static_cast<int>(rows.size());
                }
                spdlog::debug("Successfully extracted {} rows from grid {}", row_count, grid_id);

            } catch (const ComException& e) {
                spdlog::error("Failed to extract grid {} in Phase 2B: COM error: {} (HRESULT: 0x{:08X})",
                             grid_id, e.what(), e.hresult());
            } catch (const std::exception& e) {
                spdlog::error("Failed to extract grid {} in Phase 2B: {}", grid_id, e.what());
            }
        }
    }

    if (collector.extracted_count() > 0) {
        spdlog::debug("Phase 2 complete: extracted {} tree/grid elements total", collector.extracted_count());
    }
}

void ScreenReader::discover_elements(ComGuiWindowPtr window, ScreenElementCollector& collector,
                                     bool skip_trees, ScreenSearchContext* search) {
    collector.reserve(200);  // Reserve space for typical complex screens

    try {
        std::string window_id = window->get_id();
        int child_count = window->get_child_count();
        spdlog::debug("Window {} has {} top-level children, starting recursive traversal (skip_trees={})",
                     window_id, child_count, skip_trees);

        // FIRST: Explicitly probe for toolbar and title bar (often missed by Children collection)
        // These are critical for button discovery
        try {
            // Try toolbar: wnd[N]/tbar[0], wnd[N]/tbar[1], etc.
            for (int i = 0; i < 3; ++i) {  // Most windows have 0-2 toolbars
                if (search && search->stopped) break;
                try {
                    std::string tbar_id = window_id + "/tbar[" + std::to_string(i) + "]";
                    auto tbar = session_->find_element_by_id(tbar_id);
                    if (tbar) {
                        spdlog::debug("Found toolbar via ID: {}", tbar_id);
                        traverse_element_tree(tbar, collector, 0, skip_trees, search);
                    }
                } catch (const std::exception&) {
                    break;  // No more toolbars
                }
            }
        } catch (const std::exception&) {
            // Toolbar probing failed
        }

        if (search && search->stopped) return;
        try {
            // Try title bar: wnd[N]/titl
            std::string titl_id = window_id + "/titl";
            auto titl = session_->find_element_by_id(titl_id);
            if (titl) {
                spdlog::debug("Found title bar via ID: {}", titl_id);
                traverse_element_tree(titl, collector, 0, skip_trees, search);
            }
        } catch (const std::exception&) {
            // Title bar not found
        }

        // SECOND: Recursively traverse all top-level children (covers usr, mbar, etc.)
        // Fast path: one enumeration instead of get_child(i) per index.
        int visited = 0;
        bool enumerated_ok = false;
        if (child_count > 0) {
            try {
                enumerated_ok = window->children().for_each(
                    [&](const ComGuiElementPtr& child) {
                        if (visited >= child_count) return false;
                        if (search && search->stopped) return false;
                        ++visited;
                        try {
                            traverse_element_tree(child, collector, 0, skip_trees, search);
                        } catch (const std::exception&) {
                            // Skip problematic children
                        }
                        return true;
                    });
            } catch (const std::exception&) {
                // Fall through to index-based loop if nothing was visited
            }
        }
        if (!enumerated_ok || visited == 0) {
            for (int i = 0; i < child_count; ++i) {
                if (search && search->stopped) break;
                try {
                    auto child = window->get_child(i);
                    if (child) {
                        traverse_element_tree(child, collector, 0, skip_trees, search);
                    }
                } catch (const std::exception&) {
                    // Skip problematic children
                }
            }
        }
    } catch (const SapGuiException& e) {
        spdlog::warn("SapGuiException during element tree traversal: {}", e.what());
    } catch (const ComException& e) {
        spdlog::warn("ComException during element tree traversal: {}", e.what());
    } catch (const std::exception& e) {
        spdlog::warn("std::exception during element tree traversal: {}", e.what());
    }

    spdlog::info("Discovered {} unique elements via recursive traversal", collector.size());

    if (search) return;  // Search only needs control identities, never grid/tree contents.

    extract_collected_trees_and_grids(collector);
}

json ScreenReader::extract_metadata_for_collector(const ScreenElementCollector& collector) {
    auto element_ids = collector.element_ids();

    const size_t MAX_ELEMENTS = 500;
    if (element_ids.size() > MAX_ELEMENTS) {
        spdlog::warn("Element count ({}) exceeds limit ({}), truncating to prevent timeout",
                    element_ids.size(), MAX_ELEMENTS);
        element_ids.resize(MAX_ELEMENTS);
    }

    json elements = json::array();
    int elem_index = 0;
    auto extraction_start = std::chrono::high_resolution_clock::now();
    const auto MAX_EXTRACTION_TIME = std::chrono::seconds(15);

    try {
        for (const auto& elem_id : element_ids) {
            auto now = std::chrono::high_resolution_clock::now();
            auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - extraction_start);
            if (elapsed > MAX_EXTRACTION_TIME) {
                spdlog::error("Screen read timeout after {}s (processed {}/{} elements)",
                             elapsed.count(), elem_index, element_ids.size());
                break;
            }

            // Skip elements that were already extracted as grids in Phase 2B
            if (collector.is_grid_id(elem_id)) {
                spdlog::debug("read: Skipping Phase 3 extraction for grid element {} (already extracted in Phase 2B)", elem_id);
                elem_index++;
                continue;
            }

            try {
                if ((elem_index + 1) % 50 == 0 || elem_index == 0) {
                    spdlog::info("read: Extracting metadata for element {}/{}", elem_index + 1, element_ids.size());
                }

                auto elem = session_->find_element_by_id(elem_id);
                if (!elem) {
                    elem_index++;
                    continue;
                }

                std::string debug_type = "UNKNOWN";
                try {
                    // Type/id were read during Phase 1; seed the fresh object instead of
                    // re-reading them over COM.
                    const std::string& known_type = collector.known_type(elem_id);
                    if (!known_type.empty()) elem->prime_identity(elem_id, known_type);
                    debug_type = elem->get_type();
                    spdlog::trace("read: Element {}/{} ID: {} (type: {})", elem_index + 1, element_ids.size(), elem_id, debug_type);
                } catch (...) {
                    elem_index++;
                    continue;
                }

                bool is_shellcont = false;
                auto last_slash = elem_id.rfind('/');
                std::string base_id = (last_slash != std::string::npos) ? elem_id.substr(last_slash + 1) : elem_id;
                if (base_id.rfind("shellcont", 0) == 0) {
                    is_shellcont = true;
                }

                if (debug_type == "GuiContainerShell" ||
                    debug_type == "GuiCustomControl" ||
                    debug_type == "GuiSplitterShell" ||
                    debug_type == "GuiSplitterContainer" ||
                    debug_type == "GuiDockShell" ||
                    debug_type == "GuiContainerCtrl" ||
                    is_shellcont) {
                    spdlog::warn("read: Skipping container element {}/{} ({}): {} - containers not extractable",
                               elem_index + 1, element_ids.size(), debug_type, elem_id);
                    elem_index++;
                    continue;
                }

                spdlog::debug("read: Extracting metadata for element {}/{}: {} (type: {})",
                             elem_index + 1, element_ids.size(), elem_id, debug_type);

                json elem_data = ElementMetadataExtractor::extract(elem);
                if (!elem_data.is_null()) {
                    elements.push_back(elem_data);
                }
                elem_index++;
            } catch (const SapGuiException& e) {
                spdlog::warn("SapGuiException extracting element {}: {}", elem_index + 1, e.what());
                elem_index++;
            } catch (const ComException& e) {
                spdlog::warn("ComException extracting element {}: {}", elem_index + 1, e.what());
                elem_index++;
            } catch (const std::exception& e) {
                spdlog::warn("std::exception extracting element {}: {}", elem_index + 1, e.what());
                elem_index++;
            }
        }
    } catch (const std::exception& e) {
        spdlog::error("Fatal exception during metadata extraction loop: {}", e.what());
    }

    // Merge pre-extracted tree data (GuiShell elements extracted during traversal)
    const auto& extracted_data = collector.get_extracted_data();
    if (!extracted_data.empty()) {
        spdlog::info("Merging {} pre-extracted tree elements", extracted_data.size());
        for (const auto& [elem_id, tree_json] : extracted_data) {
            elements.push_back(tree_json);
        }
    }

    collapse_label_duplicates(elements);
    return elements;
}

Result ScreenReader::find(const ScreenFindOptions& query) {
    const auto start = std::chrono::high_resolution_clock::now();
    Result result;
    if (query.limit < 1 || query.limit > 100) {
        result.status = Result::Status::Error;
        result.error["code"] = "INVALID_FIND_LIMIT";
        result.error["message"] = "Find limit must be between 1 and 100";
        return result;
    }
    if (query.id_contains.empty() && query.name_contains.empty() && query.type.empty()) {
        result.status = Result::Status::Error;
        result.error["code"] = "EMPTY_FIND_QUERY";
        result.error["message"] = "Specify --id-contains, --name-contains, or --type";
        return result;
    }
    if (!session_) {
        result.status = Result::Status::Error;
        result.error["code"] = "NO_SESSION";
        result.error["message"] = "No active SAP session";
        return result;
    }
    try {
        auto window = session_->get_active_window();
        if (!window) {
            result.status = Result::Status::Error;
            result.error["code"] = "NO_WINDOW";
            result.error["message"] = "No active window found";
            return result;
        }
        ScreenSearchContext search{query};
        ScreenElementCollector collector;
        discover_elements(window, collector, false, &search);
        result.status = Result::Status::Success;
        result.data["screen_id"] = window->get_id();
        result.data["title"] = window->get_title();
        result.data["transaction"] = session_->get_transaction_code();
        attach_status_bar(result, read_action_status(session_));
        result.data["elements"] = search.matches;
        result.data["element_count"] = search.matches.size();
        result.data["scanned_count"] = search.scanned;
        result.data["match_limit_reached"] = search.match_limit_reached;
        result.data["scan_limit_reached"] = search.scan_limit_reached;
        result.data["hierarchy"] = group_elements_by_container(search.matches);
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - start);
    } catch (const ComException& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "COM_ERROR";
        result.error["message"] = e.what();
    } catch (const std::exception& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "EXCEPTION";
        result.error["message"] = e.what();
    }
    return result;
}

Result ScreenReader::read(bool include_structure, bool skip_trees, int max_rows) {
    auto start = std::chrono::high_resolution_clock::now();
    Result result;

    try {
        if (max_rows < 1 || max_rows > constants::MAX_REQUESTED_TABLE_ROWS) {
            result.status = Result::Status::Error;
            result.error["code"] = "INVALID_MAX_ROWS";
            result.error["message"] = "Requested row limit must be between 1 and 200";
            return result;
        }
        max_rows_ = max_rows;
        if (!session_) {
            result.status = Result::Status::Error;
            result.error["code"] = "NO_SESSION";
            result.error["message"] = "No active SAP session";
            return result;
        }

        auto window = session_->get_active_window();
        if (!window) {
            result.status = Result::Status::Error;
            result.error["code"] = "NO_WINDOW";
            result.error["message"] = "No active window found";
            return result;
        }

        result.status = Result::Status::Success;
        result.data["screen_id"] = window->get_id();
        result.data["title"] = window->get_title();
        result.data["transaction"] = session_->get_transaction_code();
        attach_status_bar(result, read_action_status(session_));
        result.data["child_count"] = window->get_child_count();

        if (include_structure) {
            // Clear extraction cache at start of each screen read
            ElementMetadataExtractor::clear_cache();
            ElementMetadataExtractor::set_skip_trees(skip_trees);

            // Discover all unique elements (O(1) deduplication)
            // Also extracts tree data immediately to avoid stale COM pointer segfaults
            ScreenElementCollector collector;
            discover_elements(window, collector, skip_trees);

            spdlog::info("Discovery complete, {} elements found", collector.size());

            json elements = extract_metadata_for_collector(collector);
            redact_sensitive_report_labels(elements);

            result.data["elements"] = elements;
            result.data["element_count"] = elements.size();

            // Group elements by type for better LLM understanding
            result.data["hierarchy"] = group_elements_by_container(elements);

            // Extract trees into dedicated field for easier programmatic access
            json trees = json::array();
            if (result.data["hierarchy"].contains("other")) {
                for (const auto& elem : result.data["hierarchy"]["other"]) {
                    if (elem.value("type", "") == "GuiShell" &&
                        elem.contains("tree_nodes") && !elem["tree_nodes"].empty()) {
                        trees.push_back(elem);
                    }
                }
            }
            if (!trees.empty()) {
                result.data["trees"] = trees;
                spdlog::info("Extracted {} tree elements to dedicated trees field", trees.size());
            }
        }

        auto end = std::chrono::high_resolution_clock::now();
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
        spdlog::info("Read screen with {} elements (duration: {}ms)",
                     result.data.value("element_count", 0), result.duration.count());

    } catch (const ComException& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "COM_ERROR";
        result.error["message"] = e.what();
        spdlog::error("Screen read failed: {}", e.what());
    } catch (const std::exception& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "EXCEPTION";
        result.error["message"] = e.what();
    }

    return result;
}

std::vector<json> ScreenReader::select_tabs(const std::vector<json>& tabs,
                                            const std::string& only_tab) {
    if (only_tab.empty()) return tabs;
    std::vector<json> selected;
    for (const auto& tab : tabs) {
        const std::string id = tab.value("id", "");
        if (id.empty()) continue;
        const bool exact = id == only_tab;
        const bool suffix = id.size() > only_tab.size() &&
            id.compare(id.size() - only_tab.size(), only_tab.size(), only_tab) == 0 &&
            id[id.size() - only_tab.size() - 1] == '/';
        if (exact || suffix) selected.push_back(tab);
    }
    return selected;
}

namespace {

std::string trim_blanks(const std::string& text) {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return std::string();
    return text.substr(first, text.find_last_not_of(" \t\r\n") - first + 1);
}

// Split '%_<base>_%_APP_%-TEXT' / '-TO_TEXT' into base and kind. Returns false when
// the name is not a selection-screen label carrier.
bool parse_selection_label_name(const std::string& name, std::string& base, bool& is_to_text) {
    static const std::string prefix = "%_";
    static const std::string text_suffix = "_%_APP_%-TEXT";
    static const std::string to_suffix = "_%_APP_%-TO_TEXT";
    auto ends_with = [&](const std::string& suffix) {
        return name.size() > prefix.size() + suffix.size() &&
               name.compare(name.size() - suffix.size(), suffix.size(), suffix) == 0;
    };
    if (name.rfind(prefix, 0) != 0) return false;
    if (ends_with(to_suffix)) {
        is_to_text = true;
        base = name.substr(prefix.size(), name.size() - prefix.size() - to_suffix.size());
        return true;
    }
    if (ends_with(text_suffix)) {
        is_to_text = false;
        base = name.substr(prefix.size(), name.size() - prefix.size() - text_suffix.size());
        return true;
    }
    return false;
}

} // namespace

void ScreenReader::collapse_label_duplicates(json& elements) {
    if (!elements.is_array()) return;

    // Trimmed labels of real fields, globally (legacy GuiLabel rule) and per field name.
    std::unordered_set<std::string> field_labels;
    std::vector<std::pair<std::string, std::string>> named_labels;  // field name, trimmed label
    for (const auto& elem : elements) {
        if (!elem.is_object() || elem.value("type", "") == "GuiLabel") continue;
        const std::string label = trim_blanks(elem.value("label", ""));
        if (label.empty()) continue;
        field_labels.insert(label);
        std::string base;
        bool is_to = false;
        const std::string name = elem.value("name", "");
        if (!name.empty() && !parse_selection_label_name(name, base, is_to)) {
            named_labels.emplace_back(name, label);
        }
    }
    auto sibling_has_label = [&](const std::string& base, const std::string& text) {
        for (const auto& [name, label] : named_labels) {
            const bool same_base = name == base ||
                (name.size() > base.size() && name.compare(0, base.size(), base) == 0 &&
                 name[base.size()] == '-');
            if (same_base && label == text) return true;
        }
        return false;
    };

    std::vector<std::pair<std::string, std::string>> to_carriers;  // base, trimmed "to" text
    json kept = json::array();
    for (auto& elem : elements) {
        if (elem.is_object()) {
            std::string base;
            bool is_to_text = false;
            if (parse_selection_label_name(elem.value("name", ""), base, is_to_text)) {
                const std::string trimmed = trim_blanks(elem.value("text", ""));
                bool drop = false;
                if (is_to_text) {
                    drop = true;  // only ever holds the "to" range separator
                    to_carriers.emplace_back(base, trimmed);
                } else if (trimmed.empty() || sibling_has_label(base, trimmed)) {
                    drop = true;
                } else if (elem.value("type", "") == "GuiLabel" && field_labels.count(trimmed)) {
                    drop = true;  // legacy GuiLabel rule: label repeated on some field
                }
                if (drop) continue;
            }
        }
        kept.push_back(std::move(elem));
    }
    // Range HIGH fields inherit the group caption; "to" moves to range_part.
    for (const auto& [base, to_text] : to_carriers) {
        json* low = nullptr;
        json* high = nullptr;
        for (auto& elem : kept) {
            if (!elem.is_object()) continue;
            const std::string name = elem.value("name", "");
            if (name == base + "-LOW") low = &elem;
            else if (name == base + "-HIGH") high = &elem;
        }
        if (!low || !high) continue;
        if (trim_blanks(high->value("label", "")) != to_text) continue;
        const std::string caption = trim_blanks(low->value("label", ""));
        if (caption.empty()) continue;
        (*high)["label"] = caption;
        (*high)["range_part"] = to_text;
    }
    elements = std::move(kept);
}

namespace {

constexpr int kTabPollMs = 20;
constexpr int kTabWaitMs = 5000;
int g_tab_wait_override_ms = 0;

// Poll the session until it is no longer busy. False on timeout.
bool wait_until_idle(const ComGuiSessionPtr& session) {
    const int limit_ms = g_tab_wait_override_ms > 0 ? g_tab_wait_override_ms : kTabWaitMs;
    for (int waited = 0; session->is_busy(); waited += kTabPollMs) {
        if (waited >= limit_ms) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(kTabPollMs));
    }
    return true;
}

void restore_tab(const ComGuiSessionPtr& session, const std::string& tab_id) {
    auto tab = session->find_element_by_id(tab_id);
    if (!tab) throw ComException("Original tab is no longer available");
    tab->select();
    if (!wait_until_idle(session)) throw ComException("Timed out restoring original tab");
}

std::string tab_strip_id(const std::string& tab_id) {
    const auto slash = tab_id.rfind('/');
    return slash == std::string::npos ? std::string() : tab_id.substr(0, slash);
}

struct LocatedTabStrip {
    std::string id;
    std::vector<json> tabs;  // {id, type, text, strip_id}
};

void collect_tab_strip(const ComGuiElementPtr& strip, std::vector<LocatedTabStrip>& out) {
    LocatedTabStrip info;
    info.id = strip->get_id();
    for (const auto& known : out) if (known.id == info.id) return;
    auto children = strip->children();
    for (const auto& tab : enumerate_collection(children, children.count())) {
        json entry = {{"id", tab->get_id()}, {"type", "GuiTab"}, {"strip_id", info.id}};
        try { entry["text"] = tab->get_string_property(L"Text"); } catch (const std::exception&) {}
        info.tabs.push_back(std::move(entry));
    }
    out.push_back(std::move(info));
}

} // namespace

void set_tab_wait_timeout_ms_for_testing(int ms) { g_tab_wait_override_ms = ms; }

const char* tab_read_failure_reason(TabReadStatus status) {
    switch (status) {
        case TabReadStatus::NotFound: return "not_found";
        case TabReadStatus::BusyTimeout: return "busy_timeout";
        default: return "error";
    }
}

Result ScreenReader::read_tab(const std::string& only_tab, bool skip_trees, int max_rows) {
    TraceGuard trace("ScreenReader::read_tab");
    const auto start = std::chrono::high_resolution_clock::now();
    Result result;

    try {
        if (max_rows < 1 || max_rows > constants::MAX_REQUESTED_TABLE_ROWS) {
            result.status = Result::Status::Error;
            result.error["code"] = "INVALID_MAX_ROWS";
            result.error["message"] = "Requested row limit must be between 1 and 200";
            return result;
        }
        max_rows_ = max_rows;
        if (!session_) {
            result.status = Result::Status::Error;
            result.error["code"] = "NO_SESSION";
            result.error["message"] = "No active SAP session";
            return result;
        }
        auto window = session_->get_active_window();
        if (!window) {
            result.status = Result::Status::Error;
            result.error["code"] = "NO_WINDOW";
            result.error["message"] = "No active window found";
            return result;
        }

        // Header exactly as read() produces it.
        const std::string window_id = window->get_id();
        result.status = Result::Status::Success;
        result.data["screen_id"] = window_id;
        result.data["title"] = window->get_title();
        result.data["transaction"] = session_->get_transaction_code();
        attach_status_bar(result, read_action_status(session_));
        result.data["child_count"] = window->get_child_count();

        ElementMetadataExtractor::clear_cache();
        ElementMetadataExtractor::set_skip_trees(skip_trees);

        // Cheap strip lookup: wnd/usr children, one more level into subscreen containers.
        std::vector<LocatedTabStrip> strips;
        try {
            auto usr = session_->find_element_by_id(window_id + "/usr");
            const int kMaxScanned = 200;
            auto scan = [&](auto&& self, const ComGuiElementPtr& container, bool descend) -> void {
                auto children = container->children();
                for (const auto& child : enumerate_collection(children, std::min(children.count(), kMaxScanned))) {
                    const std::string type = child->get_type();
                    if (type == "GuiTabStrip") {
                        collect_tab_strip(child, strips);
                    } else if (descend && (type == "GuiSimpleContainer" ||
                                           type == "GuiScrollContainer" || type == "GuiSubScreen")) {
                        self(self, child, false);
                    }
                }
            };
            if (usr) scan(scan, usr, true);
        } catch (const std::exception& e) {
            spdlog::debug("read_tab: cheap tab strip lookup failed: {}", e.what());
        }

        auto all_tabs = [&]() {
            std::vector<json> tabs;
            for (const auto& strip : strips) tabs.insert(tabs.end(), strip.tabs.begin(), strip.tabs.end());
            return tabs;
        };
        std::vector<json> matched = select_tabs(all_tabs(), only_tab);

        if (matched.empty()) {
            // Fallback: nested or unusual layouts. Reuse the discovery path with a type filter.
            ScreenFindOptions query;
            query.type = "GuiTabStrip";
            query.limit = 100;
            ScreenSearchContext search{query};
            ScreenElementCollector probe_collector;
            discover_elements(window, probe_collector, skip_trees, &search);
            for (const auto& match : search.matches) {
                try {
                    auto strip = session_->find_element_by_id(match.value("id", ""));
                    if (strip) collect_tab_strip(strip, strips);
                } catch (const std::exception&) {}
            }
            matched = select_tabs(all_tabs(), only_tab);
        }

        if (matched.empty()) {
            json available = json::array();
            for (const auto& tab : all_tabs()) available.push_back(tab.value("id", ""));
            result.data = json::object();
            result.status = Result::Status::Error;
            result.error["code"] = "TAB_NOT_FOUND";
            result.error["message"] = "No tab matches '" + only_tab + "'";
            result.error["available_tabs"] = available;
            return result;
        }

        // ERR-113: a read is observational. Snapshot only the strips that own the requested
        // tabs, and put them back afterwards.
        std::vector<std::string> owner_strips;
        for (const auto& tab : matched) {
            const std::string strip_id = tab.value("strip_id", "");
            if (std::find(owner_strips.begin(), owner_strips.end(), strip_id) == owner_strips.end())
                owner_strips.push_back(strip_id);
        }
        json owner_json = json::array();
        for (const auto& id : owner_strips) owner_json.push_back({{"id", id}, {"type", "GuiTabStrip"}});
        const auto selected_tabs = TabSelectionSnapshot::capture(owner_json, [&](const std::string& strip_id) {
            auto strip = session_->find_element_by_id(strip_id);
            if (!strip) return std::string();
            auto selected_dispatch = strip->get_dispatch_property(L"SelectedTab");
            if (!selected_dispatch) return std::string();
            return ComGuiElement::create(selected_dispatch)->get_id();
        });
        if (selected_tabs.selected_ids.size() != owner_strips.size()) {
            result.status = Result::Status::Error;
            result.error["code"] = "TAB_SELECTION_UNAVAILABLE";
            result.error["message"] = "Cannot expand tabs without knowing which tab to restore";
            return result;
        }
        std::map<std::string, std::string> current_selected;
        for (const auto& selected_id : selected_tabs.selected_ids)
            current_selected[tab_strip_id(selected_id)] = selected_id;

        // Header elements: the owning strip(s) and their tab headers, as found.
        ScreenElementCollector header_collector;
        for (const auto& strip : strips) {
            if (std::find(owner_strips.begin(), owner_strips.end(), strip.id) == owner_strips.end()) continue;
            if (header_collector.add_id(strip.id)) header_collector.set_known_type(strip.id, "GuiTabStrip");
            for (const auto& tab : strip.tabs) {
                const std::string id = tab.value("id", "");
                if (header_collector.add_id(id)) header_collector.set_known_type(id, "GuiTab");
            }
        }
        json elements = extract_metadata_for_collector(header_collector);

        json tabs_content = json::array();
        json tabs_failed = json::array();
        for (size_t i = 0; i < matched.size(); ++i) {
            const auto& tab = matched[i];
            const std::string tab_id = tab.value("id", "");
            const std::string tab_name = tab.value("text", "Unnamed Tab");
            const std::string strip_id = tab.value("strip_id", "");
            try {
                const bool needs_select =
                    plan_tab_selection(current_selected[strip_id], tab_id) == TabSelectionPlan::Select;
                if (needs_select) current_selected[strip_id] = tab_id;

                json tab_elements_list;
                const TabReadStatus tab_status =
                    read_tab_content(tab_id, needs_select, skip_trees, tab_elements_list);
                if (tab_status != TabReadStatus::Ok) {
                    spdlog::warn("Tab {} could not be read: {}", tab_name,
                                 tab_read_failure_reason(tab_status));
                    tabs_failed.push_back({{"tab_id", tab_id}, {"tab_name", tab_name},
                                           {"reason", tab_read_failure_reason(tab_status)}});
                    continue;
                }
                json tab_data;
                tab_data["tab_id"] = tab_id;
                tab_data["tab_name"] = tab_name;
                tab_data["tab_type"] = "GuiTab";
                tab_data["tab_index"] = i;
                tab_data["elements"] = tab_elements_list;
                tab_data["hierarchy"] = group_elements_by_container(tab_elements_list);
                tab_data["element_count"] = tab_elements_list.size();
                tabs_content.push_back(std::move(tab_data));

                std::set<std::string> present;
                for (const auto& e : elements) present.insert(e.value("id", ""));
                for (const auto& e : tab_elements_list)
                    if (!e.contains("id") || present.insert(e.value("id", "")).second) elements.push_back(e);
            } catch (const std::exception& e) {
                spdlog::warn("Exception expanding tab {}: {}", tab_name, e.what());
                tabs_failed.push_back({{"tab_id", tab_id}, {"tab_name", tab_name},
                                       {"reason", "error"}});
            }
        }

        TabSelectionSnapshot to_restore;
        for (const auto& original_id : selected_tabs.selected_ids)
            if (current_selected[tab_strip_id(original_id)] != original_id)
                to_restore.selected_ids.push_back(original_id);
        if (!to_restore.restore([&](const std::string& tab_id) { restore_tab(session_, tab_id); })) {
            result.status = Result::Status::Error;
            result.error["code"] = "TAB_RESTORE_FAILED";
            result.error["message"] = "Screen read could not restore the original tab";
            return result;
        }

        if (tabs_content.empty() && !tabs_failed.empty()) {
            const auto& first = tabs_failed.at(0);
            result.data = json::object();
            result.status = Result::Status::Error;
            result.error["code"] = "TAB_LOAD_FAILED";
            result.error["message"] = "Tab '" + first.value("tab_id", "") + "' could not be read: " +
                                      first.value("reason", "error");
            result.error["tab_id"] = first.value("tab_id", "");
            result.error["reason"] = first.value("reason", "error");
            result.error["tabs_failed"] = tabs_failed;
            return result;
        }

        redact_sensitive_report_labels(elements);
        result.data["elements"] = elements;
        result.data["element_count"] = elements.size();
        result.data["hierarchy"] = group_elements_by_container(elements);
        json trees = json::array();
        if (result.data["hierarchy"].contains("other")) {
            for (const auto& elem : result.data["hierarchy"]["other"]) {
                if (elem.value("type", "") == "GuiShell" &&
                    elem.contains("tree_nodes") && !elem["tree_nodes"].empty()) {
                    trees.push_back(elem);
                }
            }
        }
        if (!trees.empty()) result.data["trees"] = trees;
        result.data["tabs_content"] = tabs_content;
        result.data["tabs_expanded"] = true;
        result.data["expanded_tab_count"] = tabs_content.size();
        if (!tabs_failed.empty()) result.data["tabs_failed"] = tabs_failed;
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::high_resolution_clock::now() - start);
        spdlog::info("Read tab {} (duration: {}ms)", only_tab, result.duration.count());
    } catch (const ComException& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "COM_ERROR";
        result.error["message"] = e.what();
    } catch (const std::exception& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "EXCEPTION";
        result.error["message"] = e.what();
    }
    return result;
}

TabReadStatus ScreenReader::read_tab_content(const std::string& tab_id, bool needs_select,
                                             bool skip_trees, json& elements) {
    // find_element_by_id throws for an id that no longer resolves; both that and a null
    // result mean the tab is gone.
    auto find_tab = [&]() -> ComGuiElementPtr {
        try {
            return session_->find_element_by_id(tab_id);
        } catch (const std::exception& e) {
            spdlog::debug("read_tab_content: tab {} not found: {}", tab_id, e.what());
            return nullptr;
        }
    };
    auto tab_elem = find_tab();
    if (!tab_elem) return TabReadStatus::NotFound;
    if (needs_select) {
        tab_elem->select();
        if (!wait_until_idle(session_)) return TabReadStatus::BusyTimeout;
        // The pre-select pointer can be stale: traversing it yields nothing. Re-fetch.
        tab_elem = find_tab();
        if (!tab_elem) return TabReadStatus::NotFound;
    }

    ScreenElementCollector tab_collector;
    traverse_element_tree(tab_elem, tab_collector, 0, skip_trees);
    extract_collected_trees_and_grids(tab_collector);
    elements = extract_metadata_for_collector(tab_collector);

    // The tab subtree is authoritative when it yields anything besides the tab container
    // itself. A tab that reports children but exposes nothing extractable falls back to the
    // window user area, as before (decided by the extracted result, not by child count).
    bool only_container = elements.empty();
    if (!only_container && elements.size() == 1)
        only_container = elements.at(0).value("id", "") == tab_id;
    if (only_container) {
        spdlog::debug("Tab {} yielded no content, probing window user area", tab_id);
        auto active_wnd = session_->get_active_window();
        if (active_wnd) {
            auto usr = session_->find_element_by_id(active_wnd->get_id() + "/usr");
            if (usr) {
                ScreenElementCollector usr_collector;
                traverse_element_tree(usr, usr_collector, 0, skip_trees);
                extract_collected_trees_and_grids(usr_collector);
                json fallback = extract_metadata_for_collector(usr_collector);
                if (!fallback.empty() || elements.empty()) elements = std::move(fallback);
            }
        }
    }
    redact_sensitive_report_labels(elements);
    return TabReadStatus::Ok;
}

Result ScreenReader::read_with_tabs(bool skip_trees, int max_rows, const std::string& only_tab) {
    TraceGuard trace("ScreenReader::read_with_tabs");
    auto start = std::chrono::high_resolution_clock::now();
    Result result;

    try {
        if (!session_) {
            result.status = Result::Status::Error;
            result.error["code"] = "NO_SESSION";
            result.error["message"] = "No active SAP session";
            return result;
        }

        // A single requested tab never needs the full base read plus every other tab.
        if (!only_tab.empty()) return read_tab(only_tab, skip_trees, max_rows);

        // Get initial screen structure
        Result initial_result = read(true, skip_trees, max_rows);
        if (initial_result.status != Result::Status::Success) {
            return initial_result;
        }

        json screen_data = initial_result.data;
        json tabs_content = json::array();

        // Find all GuiTab elements in the hierarchy (or under GuiTabStrip)
        std::vector<json> tab_elements;
        std::unordered_set<std::string> seen_tab_ids;
        if (screen_data.contains("hierarchy") && screen_data["hierarchy"].contains("tabs")) {
            for (const auto& tab_entry : screen_data["hierarchy"]["tabs"]) {
                std::string type = tab_entry.value("type", "");
                if (type == "GuiTab") {
                    std::string tab_id = tab_entry.value("id", "");
                    if (!tab_id.empty() && seen_tab_ids.insert(tab_id).second) {
                        tab_elements.push_back(tab_entry);
                    }
                } else if (type == "GuiTabStrip" && tab_entry.contains("children") && tab_entry["children"].is_array()) {
                    for (const auto& child_id_val : tab_entry["children"]) {
                        if (child_id_val.is_string()) {
                            std::string child_id = child_id_val.get<std::string>();
                            if (!child_id.empty() && seen_tab_ids.insert(child_id).second) {
                                json child_tab;
                                child_tab["id"] = child_id;
                                child_tab["type"] = "GuiTab";
                                try {
                                    auto child_elem = session_->find_element_by_id(child_id);
                                    if (child_elem) {
                                        child_tab["text"] = child_elem->get_text();
                                    }
                                } catch (...) {}
                                tab_elements.push_back(child_tab);
                            }
                        }
                    }
                }
            }
        }

        if (!only_tab.empty()) {
            json available = json::array();
            for (const auto& tab : tab_elements) available.push_back(tab.value("id", ""));
            tab_elements = select_tabs(tab_elements, only_tab);
            if (tab_elements.empty()) {
                result.status = Result::Status::Error;
                result.error["code"] = "TAB_NOT_FOUND";
                result.error["message"] = "No tab matches '" + only_tab + "'";
                result.error["available_tabs"] = available;
                return result;
            }
        }

        if (tab_elements.empty()) {
            spdlog::info("No tabs found in screen, returning normal screen read");
            return initial_result;
        }

        spdlog::info("Found {} tabs to expand", tab_elements.size());

        // A screen read is observational. Capture each strip's selected tab before
        // navigating through its pages, then put the user's screen back as found.
        size_t strip_count = 0;
        for (const auto& tab : screen_data["hierarchy"]["tabs"])
            if (tab.value("type", "") == "GuiTabStrip") ++strip_count;
        const auto selected_tabs = TabSelectionSnapshot::capture(
            screen_data["hierarchy"]["tabs"], [&](const std::string& strip_id) {
                auto strip = session_->find_element_by_id(strip_id);
                if (!strip) return std::string();
                auto selected_dispatch = strip->get_dispatch_property(L"SelectedTab");
                if (!selected_dispatch) return std::string();
                return ComGuiElement::create(selected_dispatch)->get_id();
            });
        if (strip_count == 0 || selected_tabs.selected_ids.size() != strip_count) {
            initial_result.status = Result::Status::Error;
            initial_result.error["code"] = "TAB_SELECTION_UNAVAILABLE";
            initial_result.error["message"] =
                "Cannot expand tabs without knowing which tab to restore";
            return initial_result;
        }

        // Track each strip's current page so an already-selected tab is not re-selected.
        std::map<std::string, std::string> current_selected;
        for (const auto& selected_id : selected_tabs.selected_ids)
            current_selected[tab_strip_id(selected_id)] = selected_id;

        // For each tab, select it and capture content
        json tabs_failed = json::array();
        for (size_t i = 0; i < tab_elements.size(); ++i) {
            const auto& tab = tab_elements[i];
            std::string tab_id = tab.value("id", "");
            std::string tab_name = tab.value("text", "Unnamed Tab");

            try {
                spdlog::debug("Selecting tab {}/{}: {} [{}]", i + 1, tab_elements.size(), tab_name, tab_id);

                // Skip select() (and its server round trip) when the tab is already the
                // strip's current page; otherwise select, wait, and re-fetch inside
                // read_tab_content so the subtree is read through a fresh COM pointer.
                const std::string strip_id = tab_strip_id(tab_id);
                const bool needs_select =
                    plan_tab_selection(current_selected[strip_id], tab_id) == TabSelectionPlan::Select;
                if (needs_select) current_selected[strip_id] = tab_id;

                // NOTE: `elements` is the tab's own subtree (plus grids/trees inside it). It no
                // longer repeats the whole window user area (tab headers, base fields) per tab.
                json tab_elements_list;
                const TabReadStatus tab_status =
                    read_tab_content(tab_id, needs_select, skip_trees, tab_elements_list);
                if (tab_status != TabReadStatus::Ok) {
                    spdlog::warn("Tab {} could not be read: {}", tab_name,
                                 tab_read_failure_reason(tab_status));
                    tabs_failed.push_back({{"tab_id", tab_id}, {"tab_name", tab_name},
                                           {"reason", tab_read_failure_reason(tab_status)}});
                    continue;
                }

                // Store tab data
                json tab_data;
                tab_data["tab_id"] = tab_id;
                tab_data["tab_name"] = tab_name;
                tab_data["tab_type"] = tab["type"];
                tab_data["tab_index"] = i;
                tab_data["elements"] = tab_elements_list;
                tab_data["hierarchy"] = group_elements_by_container(tab_elements_list);
                tab_data["element_count"] = tab_elements_list.size();

                tabs_content.push_back(tab_data);

                int elem_count = tab_data["element_count"].get<int>();
                spdlog::info("Captured tab {} with {} elements", tab_name, elem_count);

            } catch (const ComException& e) {
                spdlog::warn("Failed to expand tab {}: {}", tab_name, e.what());
                tabs_failed.push_back({{"tab_id", tab_id}, {"tab_name", tab_name},
                                       {"reason", "error"}});
                continue;
            } catch (const std::exception& e) {
                spdlog::warn("Exception expanding tab {}: {}", tab_name, e.what());
                tabs_failed.push_back({{"tab_id", tab_id}, {"tab_name", tab_name},
                                       {"reason", "error"}});
                continue;
            }
        }

        // Only strips whose page actually changed need to be put back.
        TabSelectionSnapshot to_restore;
        for (const auto& original_id : selected_tabs.selected_ids)
            if (current_selected[tab_strip_id(original_id)] != original_id)
                to_restore.selected_ids.push_back(original_id);
        if (!to_restore.restore([&](const std::string& tab_id) { restore_tab(session_, tab_id); })) {
            result.status = Result::Status::Error;
            result.error["code"] = "TAB_RESTORE_FAILED";
            result.error["message"] = "Screen read could not restore the original tab";
            return result;
        }

        if (tabs_content.empty() && !tabs_failed.empty()) {
            const auto& first = tabs_failed.at(0);
            result.status = Result::Status::Error;
            result.error["code"] = "TAB_LOAD_FAILED";
            result.error["message"] = "No tab could be read (first failure: " +
                                      first.value("tab_id", "") + ", " + first.value("reason", "error") + ")";
            result.error["tab_id"] = first.value("tab_id", "");
            result.error["reason"] = first.value("reason", "error");
            result.error["tabs_failed"] = tabs_failed;
            return result;
        }

        // Add tabs content to result
        screen_data["tabs_content"] = tabs_content;
        screen_data["tabs_expanded"] = true;
        screen_data["expanded_tab_count"] = tabs_content.size();
        if (!tabs_failed.empty()) screen_data["tabs_failed"] = tabs_failed;

        result.status = Result::Status::Success;
        result.data = screen_data;

        auto end = std::chrono::high_resolution_clock::now();
        result.duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);

        spdlog::info("Read screen with {} tabs expanded (duration: {}ms)",
                    tabs_content.size(), result.duration.count());

    } catch (const ComException& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "COM_ERROR";
        result.error["message"] = e.what();
        spdlog::error("Screen read with tabs failed: {}", e.what());
    } catch (const std::exception& e) {
        result.status = Result::Status::Error;
        result.error["code"] = "EXCEPTION";
        result.error["message"] = e.what();
        spdlog::error("Exception in read_with_tabs: {}", e.what());
    }

    return result;
}

} // namespace sap
} // namespace fairyfly

