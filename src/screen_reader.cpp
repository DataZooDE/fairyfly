#include "include/com/hresult.h"
#include "include/com/control_views.h"
#include "include/com/tree_view.h"
#include "include/shell_kind.h"
#include "include/object_tree.h"
#include "include/string_utils.h"
#include "include/config/environment.h"
#include "include/screen_reader.h"
#include "include/session_info.h"
#include "include/action_status.h"
#include "include/field_fill_info.h"
#include "include/element_metadata_extractor.h"
#include "include/screen_element_collector.h"
#include "include/table_data_extractor.h"
#include "include/sensitive_data.h"
#include "include/constants.h"
#include "include/trace.h"
#include <spdlog/spdlog.h>
#include <atomic>
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
}

bool screen_candidate_matches(const ScreenFindOptions& query,
                              const std::string& id,
                              const std::string& name,
                              const std::string& type) {
    return (query.id_contains.empty() || id.find(query.id_contains) != std::string::npos) &&
           (query.name_contains.empty() ||
            utils::lower_ascii(name).find(utils::lower_ascii(query.name_contains)) != std::string::npos) &&
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

int ScreenReader::choose_list_header_row(const std::map<int, std::set<int>>& occupied) {
    int best_row = -1;
    int best_aligned = -1;
    size_t best_cells = 0;
    for (auto candidate = occupied.begin(); candidate != occupied.end(); ++candidate) {
        if (candidate->second.size() < 2) continue;
        int aligned_rows = 0;
        for (auto later = std::next(candidate); later != occupied.end(); ++later) {
            int shared = 0;
            for (int col : later->second)
                if (candidate->second.count(col)) ++shared;
            if (shared >= 2) ++aligned_rows;
        }
        if (aligned_rows > best_aligned ||
            (aligned_rows == best_aligned && candidate->second.size() > best_cells)) {
            best_row = candidate->first;
            best_aligned = aligned_rows;
            best_cells = candidate->second.size();
        }
    }
    if (best_row >= 0) return best_row;
    // No candidate with two cells: the busiest row, as before (or -1 without rows).
    for (const auto& [row, cols] : occupied) {
        if (best_row < 0 || cols.size() > occupied.at(best_row).size()) best_row = row;
    }
    return best_row;
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

    // The same header choice as the extraction path.
    const int header_row = choose_list_header_row(occupied);
    const auto header = occupied.find(header_row);
    if (header == occupied.end() || header->second.size() < 2) return false;

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
            } else if (is_grid_shell(subtype) ||
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
                   (type == "GuiShell" && is_grid_shell(elem.value("subtype", ""))) ||
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
        auto node_keys = as_tree(*element).get_all_node_keys();
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
            columns = as_grid(*element).get_column_order();
            spdlog::debug("Tree {} has {} columns", elem_id, columns.size());
        } catch (const std::exception& e) {
            spdlog::debug("Could not get column order for {}: {}", elem_id, e.what());
        }

        // Extract each node with robust per-property error handling
        json nodes_array = json::array();
        int failed_nodes = 0;
        TreeNodeTexts node_texts(*element);

        for (size_t i = 0; i < node_keys.size(); ++i) {
            const auto& key = node_keys[i];
            try {
                spdlog::debug("Extracting node {}/{}: key='{}'", i+1, node_keys.size(), key);
                const Stopwatch node_timer;

                json node;
                node["key"] = key;

                // Extract node text with individual error handling
                try {
                    spdlog::debug("  -> Getting node text for key '{}'", key);
                    node["text"] = node_texts.text(key);
                    spdlog::debug("  -> Got node text for key '{}'", key);
                } catch (const std::exception& e) {
                    node["text"] = "[extraction_failed]";
                    spdlog::warn("Failed to extract text for node key '{}' in tree {}: {}", key, elem_id, e.what());
                }

                // Extract node path with individual error handling
                try {
                    spdlog::debug("  -> Getting node path for key '{}'", key);
                    node["path"] = as_tree(*element).get_node_path_by_key(key);
                    spdlog::debug("  -> Got node path: '{}'", node["path"].get<std::string>());
                } catch (const std::exception& e) {
                    node["path"] = "[extraction_failed]";
                    spdlog::warn("Failed to extract path for node key '{}' in tree {}: {}", key, elem_id, e.what());
                }

                // Extract column values using GetItemText
                if (!columns.empty()) {
                    spdlog::debug("  -> Extracting {} columns for key '{}'", columns.size(), key);
                    node["available_columns"] = columns;

                    const Stopwatch col_timer;
                    node["column_values"] = as_tree(*element).item_texts(key, columns, /*skip_hierarchy=*/true);
                    const auto col_ms = col_timer.elapsed().count();
                    if (col_ms > 500) {
                        spdlog::warn("  -> Column extraction for key '{}' took {}ms", key, col_ms);
                    }
                }

                nodes_array.push_back(node);

                // Log if this node took unusually long
                const auto node_ms = node_timer.elapsed().count();
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
            const int count = std::min(as_toolbar(*element).get_grid_toolbar_button_count(), 100);
            for (int i = 0; i < count; ++i) {
                const std::string button_id = as_toolbar(*element).get_grid_toolbar_button_id(i);
                if (button_id.empty()) continue; // Separator or menu gap.
                buttons.push_back({
                    {"id", elem_id + "/btn_" + button_id},
                    {"type", "GuiButton"},
                    {"name", button_id},
                    {"tooltip", as_toolbar(*element).get_grid_toolbar_button_tooltip(i)},
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

        // Header detection: the row the later rows align with (see choose_list_header_row), over the
        // non-empty non-separator cells; the busiest row when nothing aligns.
        std::map<int, std::set<int>> occupied_cells;
        for (const auto& [r, cols] : grid_map) {
            for (const auto& [c, val] : cols) {
                if (!val.empty() && !is_separator(val)) occupied_cells[r].insert(c);
            }
        }
        int header_row = choose_list_header_row(occupied_cells);
        if (header_row == -1) {
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
    return redact_positioned_label_text(label->get_string_property(L"Text"));
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

bool probe_all_from_environment() { return config::Environment::process().flag(config::env::kProbeAll); }

std::optional<TreeReaderMode> parse_tree_reader_mode(const std::string& text) {
    const std::string value = utils::lower_ascii(text);
    if (value == "legacy") return TreeReaderMode::Legacy;
    if (value == "bulk") return TreeReaderMode::Bulk;
    if (value == "auto") return TreeReaderMode::Auto;
    return std::nullopt;
}

const char* tree_reader_mode_name(TreeReaderMode mode) {
    switch (mode) {
        case TreeReaderMode::Bulk: return "bulk";
        case TreeReaderMode::Auto: return "auto";
        default: return "legacy";
    }
}

TreeReaderMode tree_reader_mode_from_environment() {
    // Bulk where available is the default; `legacy` is the explicit escape hatch.
    return parse_tree_reader_mode(config::Environment::process().get(config::env::kScreenReader)).value_or(TreeReaderMode::Auto);
}

bool tree_reader_mode_set_in_environment() {
    return parse_tree_reader_mode(config::Environment::process().get(config::env::kScreenReader)).has_value();
}

namespace {
// Process-wide switch-off of the bulk reader: one RPC_E_SERVERFAULT, or this many validator
// failures in a row (a success resets the streak).
constexpr int kMaxConsecutiveValidatorFailures = 3;
std::atomic<bool> g_bulk_reader_disabled{false};
std::atomic<int> g_validator_failures{0};

void disable_bulk_reader(const char* why) {
    if (!g_bulk_reader_disabled.exchange(true))
        spdlog::warn("screen read: object tree reader disabled for this process ({})", why);
}

void note_validator_failure() {
    if (g_validator_failures.fetch_add(1) + 1 >= kMaxConsecutiveValidatorFailures)
        disable_bulk_reader("repeated validator failures");
}
} // namespace

bool bulk_reader_disabled() { return g_bulk_reader_disabled.load(); }

void reset_bulk_reader_state_for_testing() {
    g_bulk_reader_disabled = false;
    g_validator_failures = 0;
}

TreeReaderMode ScreenReader::effective_tree_reader_mode() const {
    return tree_reader_mode_ ? *tree_reader_mode_ : tree_reader_mode_from_environment();
}

bool ScreenReader::report_screen_reader_diagnostics(const std::string& fallback_reason) const {
    // The default mode keeps the output shape of the per-element reader: diagnostics appear when a mode was
    // chosen explicitly (override or environment) or when the bulk reader failed and the read fell back.
    // Intentional legacy reads (--probe-all, reads without structure) are not failures.
    if (tree_reader_mode_ || tree_reader_mode_set_in_environment()) return true;
    return !fallback_reason.empty() && fallback_reason != "probe_all" && fallback_reason != "include_structure_false";
}

ObjectTreeSource ScreenReader::object_tree_source() const {
    if (tree_source_) return tree_source_;
    ObjectTreeSource real = [session = session_](const std::string& id, const std::vector<std::string>& props) {
        ObjectTreeFailure why = ObjectTreeFailure::None;
        auto answer = session->get_object_tree(id, props, &why);
        if (!answer && why == ObjectTreeFailure::ServerFault) throw ObjectTreeServerFault();
        return answer;
    };
    // Live robustness checks: FAIRYFLY_DIAG=1 + FAIRYFLY_BULK_FAULT=<mode> simulate a failing GetObjectTree.
    const auto& env = config::Environment::process();
    const std::string diag = env.get(config::env::kDiag), fault = env.get(config::env::kBulkFault);
    const std::string mode = injected_fault_mode(diag.empty() ? nullptr : diag.c_str(), fault.empty() ? nullptr : fault.c_str());
    return mode.empty() ? real : wrap_with_injected_fault(std::move(real), mode);
}

ElementProbeSource ScreenReader::element_probe_source() const {
    if (probe_source_) return probe_source_;
    return [session = session_](const std::string& id, bool want_acc_label, bool want_selected) {
        auto element = session->find_element_by_id(id);
        if (!element) throw ComException("Element not found: " + id);
        ElementProbe probe;
        if (want_acc_label) {
            try {
                probe.acc_label = element->get_string_property(L"AccLabel");
            } catch (const ComException&) {}  // same as ComGuiElement::get_label
        }
        if (want_selected) probe.selected = element->get_property_bool(L"Selected");
        return probe;
    };
}

namespace {

/// Types that are never walked as containers. GuiMenu is one so top-level menus are captured, but the 100+ nested
/// submenus are skipped.
bool is_known_leaf(const std::string& type) {
    return type == "GuiButton" || type == "GuiTextField" || type == "GuiCTextField" || type == "GuiPasswordField" ||
           type == "GuiRadioButton" || type == "GuiCheckBox" || type == "GuiLabel" || type == "GuiStatusbar" || type == "GuiMenu";
}

/// Types walked as containers even when they report no children (subscreens, toolbars, tab strips, group boxes, ...).
bool is_container_type(const std::string& type) {
    static const char* const kContainers[] = {"GuiContainerShell", "GuiSplitterShell", "GuiSimpleContainer", "GuiScrollContainer", "GuiUserArea", "GuiCustomControl", "GuiToolbar", "GuiToolbarControl", "GuiTitlebar", "GuiTabStrip", "GuiTab", "GuiSubScreen", "GuiBox"};
    return std::any_of(std::begin(kContainers), std::end(kContainers), [&](const char* t) { return type == t; });
}

}  // namespace

/// Shells and grids are not walked: their ids are collected for phase 2 (fresh COM pointers), a metadata shell is
/// added as it is. True when the element was handled here.
bool ScreenReader::collect_shell_or_grid(const ComGuiElementPtr& element, const std::string& type, const std::string& elem_id,
                                         ScreenElementCollector& collector, int depth, bool skip_trees) {
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
            return true;
        }

        if (extraction_kind == ShellExtractionKind::Metadata) {
            collector.add(element);
            return true;
        }

        if (skip_trees) {
            // When --skip-trees is enabled, completely skip GuiShell elements
            spdlog::debug("{}[depth={}] Skipping GuiShell element {} (--skip-trees enabled)",
                         std::string(depth * 2, ' '), depth, elem_id);
            return true;
        } else {
            // PHASE 1: Collect tree ID for later extraction with fresh COM pointer
            // Don't extract now - the traversal pointer may become stale
            // Will extract in Phase 2 after traversal completes using session->find_element_by_id()
            spdlog::debug("Found GuiShell {} (subtype='{}') at depth {} - collecting ID for Phase 2 extraction", elem_id, subtype, depth);
            collector.add_tree_id(elem_id);

            // Don't traverse children or store COM pointer - will extract later
            return true;
        }
    }

    // Check for GuiGridView / GuiTableControl - collect for Phase 2 extraction
    if (type == "GuiGridView" || type == "GuiTableControl") {
        spdlog::debug("Found grid {} (type={}) at depth {} - collecting ID for Phase 2 extraction",
                     elem_id, type, depth);
        collector.add_grid_id(elem_id);
        // Don't traverse children - grids are leaf elements with no meaningful child structure
        return true;
    }
    return false;
}

/// GuiUserArea: tabular grid cells (/lbl[col,row]) become one grid, other children are walked as usual. False: stop
/// here (searching, or no --probe-all: a user area has no /shell or /shellcont children to probe for).
bool ScreenReader::walk_user_area(const ComGuiElementPtr& element, const std::string& elem_id, int child_count,
                                  ScreenElementCollector& collector, int depth, bool skip_trees, ScreenSearchContext* search) {
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
        if (search && search->stopped) return false;

        if (!enumerated_ok || enumerated == 0) {
            for (int i = 0; i < total_items; ++i) {
                if (search && search->stopped) return false;
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
    if (search && !probe_all_ && !probe_all_from_environment()) return false;

    // GuiUserArea does not contain /shell or /shellcont container children
    if (!probe_all_ && !probe_all_from_environment()) return false;
    return true;
}

/// The Children collection (one enumeration), or by index when that yields nothing. Errors on single children are
/// skipped: the ID probing afterwards may still reach them.
void ScreenReader::walk_children(const ComGuiElementPtr& element, int child_count, ScreenElementCollector& collector, int depth,
                                 bool skip_trees, ScreenSearchContext* search) {
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
}

/// The safety net for controls FindById reaches but Children does not list (see id_probe_candidates).
void ScreenReader::probe_children_by_id(const std::string& type, const std::string& elem_id, int child_count,
                                        ScreenElementCollector& collector, int depth, bool skip_trees, ScreenSearchContext* search) {
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

    if (collect_shell_or_grid(element, type, elem_id, collector, depth, skip_trees)) return;

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
        if (is_known_leaf(type)) return;

        // Introspect child count directly from element
        int child_count = 0;
        try {
            child_count = element->get_child_count();
        } catch (...) {
            child_count = 0;
        }

        if (child_count <= 0 && !is_container_type(type)) return;  // a leaf: no children to walk

        if (type == "GuiUserArea" && !walk_user_area(element, elem_id, child_count, collector, depth, skip_trees, search))
            return;

        walk_children(element, child_count, collector, depth, skip_trees, search);
        if (search && search->stopped) return;

        probe_children_by_id(type, elem_id, child_count, collector, depth, skip_trees, search);
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
                spdlog::error("Failed to extract tree {} in Phase 2: COM error: {} (HRESULT: {})",
                             tree_id, e.what(), hresult_hex(e.hresult()));
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
                spdlog::error("Failed to extract grid {} in Phase 2B: COM error: {} (HRESULT: {})",
                             grid_id, e.what(), hresult_hex(e.hresult()));
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

json ScreenReader::extract_metadata_for_collector(const ScreenElementCollector& collector,
                                                  const TreeSnapshot* snapshot) {
    auto element_ids = collector.element_ids();
    const ElementProbeSource probe_source = snapshot ? element_probe_source() : ElementProbeSource();

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

                // Bulk path: the tree already holds this element's properties.
                if (snapshot) {
                    if (const ObjectTreeNode* node = snapshot->find(elem_id)) {
                        if (is_phase3_skipped_container(node->type, elem_id)) {
                            spdlog::warn("read: Skipping container element {}/{} ({}): {} - containers not extractable",
                                       elem_index + 1, element_ids.size(), node->type, elem_id);
                            elem_index++;
                            continue;
                        }
                        try {
                            if (auto built = build_bulk_plain_element(*snapshot, *node, probe_source)) {
                                elements.push_back(std::move(*built));
                                elem_index++;
                                continue;
                            }
                        } catch (const std::exception& e) {
                            // Element vanished or a targeted read failed: read it the legacy way.
                            spdlog::debug("read: bulk element {} falls back to the legacy read: {}", elem_id, e.what());
                        }
                    }
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

                json elem_data = metadata_.extract(elem);
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
    const Stopwatch timer;
    Result result;
    if (query.limit < 1 || query.limit > 100) {
        result.set_error("INVALID_FIND_LIMIT", "Find limit must be between 1 and 100");
        return result;
    }
    if (query.id_contains.empty() && query.name_contains.empty() && query.type.empty()) {
        result.set_error("EMPTY_FIND_QUERY", "Specify --id-contains, --name-contains, or --type");
        return result;
    }
    if (!session_) {
        result.set_error("NO_SESSION", "No active SAP session");
        return result;
    }
    try {
        auto window = session_->get_active_window();
        if (!window) {
            result.set_error("NO_WINDOW", "No active window found");
            return result;
        }
        ScreenSearchContext search{query};
        ScreenElementCollector collector;
        discover_elements(window, collector, false, &search);
        result.status = Result::Status::Success;
        result.data["screen_id"] = window->get_id();
        result.data["title"] = window->get_title();
        result.data["transaction"] = session_->get_transaction_code();
        attach_status_bar(result, read_action_status(session_, window->get_id()));
        result.data["elements"] = search.matches;
        result.data["element_count"] = search.matches.size();
        result.data["scanned_count"] = search.scanned;
        result.data["match_limit_reached"] = search.match_limit_reached;
        result.data["scan_limit_reached"] = search.scan_limit_reached;
        result.data["hierarchy"] = group_elements_by_container(search.matches);
        result.duration = timer.elapsed();
    } catch (const ComException& e) {
        result.set_error("COM_ERROR", e.what());
    } catch (const std::exception& e) {
        result.set_error("EXCEPTION", e.what());
    }
    return result;
}

bool ScreenReader::bulk_permitted(TreeReaderMode mode, std::string& reason) const {
    if (mode == TreeReaderMode::Legacy) return false;
    if (probe_all_ || probe_all_from_environment()) {
        reason = "probe_all";
        return false;
    }
    if (bulk_reader_disabled()) {
        reason = "disabled";
        return false;
    }
    return true;
}

Result ScreenReader::bulk_unavailable_error(const std::string& reason) {
    // Forced bulk never falls back silently, so a measurement cannot be the legacy path.
    Result result;
    result.status = Result::Status::Error;
    result.data = json::object();
    result.error["code"] = "OBJECT_TREE_UNAVAILABLE";
    result.error["message"] = "The bulk screen reader (FAIRYFLY_SCREEN_READER=bulk) could not read this screen: " + reason;
    result.error["reason"] = reason;
    return result;
}

void ScreenReader::attach_tab_diagnostics(Result& result, TreeReaderMode mode) const {
    if (mode == TreeReaderMode::Legacy) return;
    // read_with_tabs: the base read already reported; the tab reads add to it.
    std::set<std::string> kinds;
    std::string reason = tab_bulk_.reason;
    if (reason.empty() && result.diagnostics.contains("screen_reader")) {
        reason = result.diagnostics["screen_reader"].value("fallback_reason", "");
    }
    if (!report_screen_reader_diagnostics(reason)) return;
    if (result.diagnostics.contains("screen_reader")) {
        const json& base = result.diagnostics["screen_reader"];
        kinds.insert(base.value("used", "legacy"));
        if (reason.empty()) reason = base.value("fallback_reason", "");
    }
    if (tab_bulk_.bulk > 0) kinds.insert("bulk");
    if (tab_bulk_.legacy > 0) kinds.insert("legacy");
    const std::string used = kinds.empty() ? "legacy" : kinds.size() == 1 ? *kinds.begin() : "mixed";
    json info = {{"mode", tree_reader_mode_name(mode)}, {"used", used}};
    if (!reason.empty()) info["fallback_reason"] = reason;
    result.diagnostics["screen_reader"] = std::move(info);
}

bool ScreenReader::read_elements_bulk(const std::string& root_id, bool is_window,
                                      int root_child_count, bool skip_trees, json& elements,
                                      std::string& reason) {
    try {
        const std::string& window_id = root_id;
        const auto started = std::chrono::steady_clock::now();
        std::optional<std::string> raw;
        try {
            raw = object_tree_source()(window_id, object_tree_requested_properties());
        } catch (const ObjectTreeServerFault&) {
            disable_bulk_reader("RPC_E_SERVERFAULT");
            reason = "server_fault";
            return false;
        }
        if (!raw) {
            reason = ComGuiSession::object_tree_support() == ObjectTreeSupport::Missing
                         ? "unsupported" : "no_answer";
            return false;
        }
        const size_t bytes = raw->size();

        std::string parse_error;
        auto tree = parse_object_tree(*raw, window_id, &parse_error);
        std::string().swap(*raw);  // the raw answer (unredacted) is not kept any longer
        if (!tree) {
            note_validator_failure();
            reason = "invalid_tree: " + parse_error;
            return false;
        }
        TreeSnapshot snapshot(std::move(*tree));
        if (snapshot.root().children.size() != static_cast<size_t>(root_child_count)) {
            note_validator_failure();
            reason = "node_count_mismatch";
            return false;
        }
        g_validator_failures = 0;
        const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - started).count();
        spdlog::info("screen read: object tree 1 call, {} B, {} ms, {} nodes",
                     bytes, elapsed_ms, snapshot.size());

        ScreenElementCollector collector;
        const TreeReplayResult replay = is_window
            ? replay_discovery(snapshot, window_id, skip_trees, collector)
            : replay_subtree(snapshot, window_id, skip_trees, collector);

        // Hosts the tree cannot answer keep their legacy FindById probe (see id_probe_candidates).
        for (const auto& host : replay.probe_hosts) {
            const ObjectTreeNode* host_node = snapshot.find(host);
            const auto probes = id_probe_candidates(host_node ? host_node->type : std::string(),
                                                    host, 0, collector, false);
            std::string missed_index_family;
            for (const auto& probe_id : probes) {
                const auto bracket = probe_id.find('[');
                const std::string family = bracket == std::string::npos ? std::string() : probe_id.substr(0, bracket);
                if (!family.empty() && family == missed_index_family) continue;
                try {
                    auto probed = session_->find_element_by_id(probe_id);
                    if (probed) traverse_element_tree(probed, collector, 1, skip_trees, nullptr);
                } catch (const std::exception& e) {
                    spdlog::trace("No probed child {}: {}", probe_id, e.what());
                    if (!family.empty()) missed_index_family = family;
                }
            }
        }

        spdlog::info("Discovery complete, {} elements found", collector.size());
        extract_collected_trees_and_grids(collector);
        elements = extract_metadata_for_collector(collector, &snapshot);
        return true;
    } catch (const std::exception& e) {
        spdlog::warn("screen read: bulk reader failed, using the legacy reader: {}", e.what());
        reason = "exception";
        return false;
    }
}

Result ScreenReader::read(bool include_structure, bool skip_trees, int max_rows) {
    const Stopwatch timer;
    Result result;

    try {
        if (max_rows < 1 || max_rows > constants::MAX_REQUESTED_TABLE_ROWS) {
            result.set_error("INVALID_MAX_ROWS", "Requested row limit must be between 1 and 200");
            return result;
        }
        max_rows_ = max_rows;
        if (!session_) {
            result.set_error("NO_SESSION", "No active SAP session");
            return result;
        }

        auto window = session_->get_active_window();
        if (!window) {
            result.set_error("NO_WINDOW", "No active window found");
            return result;
        }

        result.status = Result::Status::Success;
        result.data["screen_id"] = window->get_id();
        result.data["title"] = window->get_title();
        result.data.update(screen_header_info(*session_));  // system, client, user, transaction
        attach_status_bar(result, read_action_status(session_, window->get_id()));
        result.data["child_count"] = window->get_child_count();

        const TreeReaderMode mode = effective_tree_reader_mode();
        std::string bulk_reason;
        bool used_bulk = false;

        if (include_structure) {
            // a fresh extractor (options and cache) for this screen read
            metadata_ = ElementMetadataExtractor({skip_trees});
            if (skip_trees) spdlog::info("Tree extraction disabled (--skip-trees)");

            json elements;
            if (mode != TreeReaderMode::Legacy) {
                if (bulk_permitted(mode, bulk_reason)) {
                    used_bulk = read_elements_bulk(window->get_id(), true,
                                                   result.data["child_count"].get<int>(),
                                                   skip_trees, elements, bulk_reason);
                }
                if (!used_bulk && mode == TreeReaderMode::Bulk && bulk_reason != "probe_all")
                    return bulk_unavailable_error(bulk_reason);
                if (!used_bulk) {
                    // Fresh state for the legacy read (a failed bulk attempt may have run phase 2).
                    metadata_.clear_cache();
                }
            }

            if (!used_bulk) {
                // Discover all unique elements (O(1) deduplication)
                // Also extracts tree data immediately to avoid stale COM pointer segfaults
                ScreenElementCollector collector;
                discover_elements(window, collector, skip_trees);

                spdlog::info("Discovery complete, {} elements found", collector.size());

                elements = extract_metadata_for_collector(collector);
            }
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

        if (!include_structure) bulk_reason = "include_structure_false";
        if (mode != TreeReaderMode::Legacy && report_screen_reader_diagnostics(bulk_reason)) {
            json info = {{"mode", tree_reader_mode_name(mode)}, {"used", used_bulk ? "bulk" : "legacy"}};
            if (!bulk_reason.empty()) info["fallback_reason"] = bulk_reason;
            result.diagnostics["screen_reader"] = std::move(info);
        }

        result.duration = timer.elapsed();
        spdlog::info("Read screen with {} elements (duration: {}ms)",
                     result.data.value("element_count", 0), result.duration.count());

    } catch (const ComException& e) {
        result.set_error("COM_ERROR", e.what());
        spdlog::error("Screen read failed: {}", e.what());
    } catch (const std::exception& e) {
        result.set_error("EXCEPTION", e.what());
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
        case TabReadStatus::TreeUnavailable: return "object_tree_unavailable";
        default: return "error";
    }
}

namespace {
/// The tabs of all located strips, in order.
std::vector<json> tabs_of(const std::vector<LocatedTabStrip>& strips) {
    std::vector<json> tabs;
    for (const auto& strip : strips) tabs.insert(tabs.end(), strip.tabs.begin(), strip.tabs.end());
    return tabs;
}
}  // namespace

/// The tab strips of a window, found cheaply: the children of usr, one more level into subscreen containers.
std::vector<LocatedTabStrip> ScreenReader::locate_tab_strips(const std::string& window_id) {
    std::vector<LocatedTabStrip> strips;
    // Cheap strip lookup: wnd/usr children, one more level into subscreen containers.
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
    return strips;
}

/// Nested or unusual layouts: the tab strips the discovery path finds (filtered by type) join `strips`.
void ScreenReader::add_tab_strips_by_search(const ComGuiWindowPtr& window, bool skip_trees, std::vector<LocatedTabStrip>& strips) {
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
}

namespace {

/// TAB_LOAD_FAILED when no requested tab could be read: the first failure's tab and reason, and all failures.
Result tab_load_failed(Result& result, const std::string& message, const json& tabs_failed) {
    const auto& first = tabs_failed.at(0);
    result.set_error("TAB_LOAD_FAILED", message);
    result.error["tab_id"] = first.value("tab_id", "");
    result.error["reason"] = first.value("reason", "error");
    result.error["tabs_failed"] = tabs_failed;
    return result;
}

}  // namespace

/// The id of the strip's selected tab ("" when the strip or its SelectedTab cannot be read).
std::string ScreenReader::selected_tab_of(const std::string& strip_id) const {
    auto strip = session_->find_element_by_id(strip_id);
    if (!strip) return std::string();
    auto selected_dispatch = strip->get_dispatch_property(L"SelectedTab");
    if (!selected_dispatch) return std::string();
    return ComGuiElement::create(selected_dispatch)->get_id();
}

/// Reads one tab of a tab read: selects it unless it is already its strip's page (`current_selected` follows), reads
/// its subtree into `tab_elements_list` and appends it to `tabs_content` (index `index`); a tab that cannot be read
/// goes to `tabs_failed` with its reason. false then.
bool ScreenReader::expand_tab(const json& tab, size_t index, const std::string& strip_id, bool skip_trees,
                              std::map<std::string, std::string>& current_selected, json& tabs_content,
                              json& tabs_failed, json& tab_elements_list) {
    const std::string tab_id = tab.value("id", "");
    const std::string tab_name = tab.value("text", "Unnamed Tab");
    try {
        // Skip select() (and its server round trip) when the tab is already the strip's current page; otherwise
        // select, wait, and re-fetch inside read_tab_content so the subtree is read through a fresh COM pointer.
        const bool needs_select = plan_tab_selection(current_selected[strip_id], tab_id) == TabSelectionPlan::Select;
        if (needs_select) current_selected[strip_id] = tab_id;

        // `elements` is the tab's own subtree (plus grids/trees inside it), not the whole window user area per tab.
        const TabReadStatus tab_status = read_tab_content(tab_id, needs_select, skip_trees, tab_elements_list);
        if (tab_status != TabReadStatus::Ok) {
            spdlog::warn("Tab {} could not be read: {}", tab_name, tab_read_failure_reason(tab_status));
            tabs_failed.push_back({{"tab_id", tab_id}, {"tab_name", tab_name},
                                   {"reason", tab_read_failure_reason(tab_status)}});
            return false;
        }
        json tab_data;
        tab_data["tab_id"] = tab_id;
        tab_data["tab_name"] = tab_name;
        tab_data["tab_type"] = "GuiTab";
        tab_data["tab_index"] = index;
        tab_data["elements"] = tab_elements_list;
        tab_data["hierarchy"] = group_elements_by_container(tab_elements_list);
        tab_data["element_count"] = tab_elements_list.size();
        spdlog::info("Captured tab {} with {} elements", tab_name, tab_elements_list.size());
        tabs_content.push_back(std::move(tab_data));
        return true;
    } catch (const std::exception& e) {
        spdlog::warn("Exception expanding tab {}: {}", tab_name, e.what());
        tabs_failed.push_back({{"tab_id", tab_id}, {"tab_name", tab_name}, {"reason", "error"}});
        return false;
    }
}

/// Puts back every strip whose page a tab read changed (a screen read is observational). false when that failed.
bool ScreenReader::restore_tab_selection(const TabSelectionSnapshot& original,
                                         std::map<std::string, std::string>& current_selected) {
    TabSelectionSnapshot to_restore;
    for (const auto& original_id : original.selected_ids)
        if (current_selected[tab_strip_id(original_id)] != original_id)
            to_restore.selected_ids.push_back(original_id);
    return to_restore.restore([&](const std::string& tab_id) { restore_tab(session_, tab_id); });
}

/// The tabs of a screen read's hierarchy: its GuiTab entries and the children of its GuiTabStrip entries (with their
/// text), each id once.
std::vector<json> ScreenReader::tabs_in_hierarchy(const json& screen_data) const {
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
    return tab_elements;
}

Result ScreenReader::read_tab(const std::string& only_tab, bool skip_trees, int max_rows) {
    TraceGuard trace("ScreenReader::read_tab");
    const Stopwatch timer;
    Result result;

    try {
        if (max_rows < 1 || max_rows > constants::MAX_REQUESTED_TABLE_ROWS) {
            result.set_error("INVALID_MAX_ROWS", "Requested row limit must be between 1 and 200");
            return result;
        }
        max_rows_ = max_rows;
        tab_bulk_ = TabBulkStats();
        if (!session_) {
            result.set_error("NO_SESSION", "No active SAP session");
            return result;
        }
        auto window = session_->get_active_window();
        if (!window) {
            result.set_error("NO_WINDOW", "No active window found");
            return result;
        }

        // Header exactly as read() produces it.
        const std::string window_id = window->get_id();
        result.status = Result::Status::Success;
        result.data["screen_id"] = window_id;
        result.data["title"] = window->get_title();
        result.data.update(screen_header_info(*session_));  // system, client, user, transaction
        attach_status_bar(result, read_action_status(session_, window_id));
        result.data["child_count"] = window->get_child_count();

        metadata_ = ElementMetadataExtractor({skip_trees});
        if (skip_trees) spdlog::info("Tree extraction disabled (--skip-trees)");

        std::vector<LocatedTabStrip> strips = locate_tab_strips(window_id);
        std::vector<json> matched = select_tabs(tabs_of(strips), only_tab);

        if (matched.empty()) {   // nested or unusual layouts: the discovery path, filtered by type
            add_tab_strips_by_search(window, skip_trees, strips);
            matched = select_tabs(tabs_of(strips), only_tab);
        }

        if (matched.empty()) {
            json available = json::array();
            for (const auto& tab : tabs_of(strips)) available.push_back(tab.value("id", ""));
            result.data = json::object();
            result.set_error("TAB_NOT_FOUND", "No tab matches '" + only_tab + "'");
            result.error["tab_id"] = only_tab;
            result.error["reason"] = "not_found";
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
        const auto selected_tabs = TabSelectionSnapshot::capture(
            owner_json, [&](const std::string& strip_id) { return selected_tab_of(strip_id); });
        if (selected_tabs.selected_ids.size() != owner_strips.size()) {
            result.set_error("TAB_SELECTION_UNAVAILABLE", "Cannot expand tabs without knowing which tab to restore");
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
            json tab_elements_list;
            if (!expand_tab(matched[i], i, matched[i].value("strip_id", ""), skip_trees, current_selected, tabs_content,
                            tabs_failed, tab_elements_list))
                continue;
            std::set<std::string> present;
            for (const auto& e : elements) present.insert(e.value("id", ""));
            for (const auto& e : tab_elements_list)
                if (!e.contains("id") || present.insert(e.value("id", "")).second) elements.push_back(e);
        }

        if (!restore_tab_selection(selected_tabs, current_selected)) {
            result.set_error("TAB_RESTORE_FAILED", "Screen read could not restore the original tab");
            return result;
        }
        if (!tab_bulk_.unavailable.empty()) return bulk_unavailable_error(tab_bulk_.unavailable);

        if (tabs_content.empty() && !tabs_failed.empty()) {
            const auto& first = tabs_failed.at(0);
            result.data = json::object();
            return tab_load_failed(result, "Tab '" + first.value("tab_id", "") + "' could not be read: " +
                                               first.value("reason", "error"), tabs_failed);
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
        attach_tab_diagnostics(result, effective_tree_reader_mode());
        result.duration = timer.elapsed();
        spdlog::info("Read tab {} (duration: {}ms)", only_tab, result.duration.count());
    } catch (const ComException& e) {
        result.set_error("COM_ERROR", e.what());
    } catch (const std::exception& e) {
        result.set_error("EXCEPTION", e.what());
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

    // One GetObjectTree call for the subtree when the bulk reader is on; otherwise (or when it
    // cannot serve this read) the element by element traversal. False: forced Bulk failed.
    const TreeReaderMode mode = effective_tree_reader_mode();
    auto read_subtree = [&](const ComGuiElementPtr& root, const std::string& root_id, json& out) {
        if (mode != TreeReaderMode::Legacy) {
            std::string reason;
            bool ok = false;
            if (bulk_permitted(mode, reason)) {
                try {
                    ok = read_elements_bulk(root_id, false, root->get_child_count(), skip_trees, out, reason);
                } catch (const std::exception&) {
                    reason = "exception";
                }
            }
            if (ok) {
                ++tab_bulk_.bulk;
                return true;
            }
            if (mode == TreeReaderMode::Bulk && reason != "probe_all") {
                tab_bulk_.unavailable = reason;
                return false;
            }
            ++tab_bulk_.legacy;
            if (tab_bulk_.reason.empty()) tab_bulk_.reason = reason;
        }
        ScreenElementCollector collector;
        traverse_element_tree(root, collector, 0, skip_trees);
        extract_collected_trees_and_grids(collector);
        out = extract_metadata_for_collector(collector);
        return true;
    };
    if (!read_subtree(tab_elem, tab_id, elements)) return TabReadStatus::TreeUnavailable;

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
            const std::string usr_id = active_wnd->get_id() + "/usr";
            auto usr = session_->find_element_by_id(usr_id);
            if (usr) {
                json fallback;
                if (!read_subtree(usr, usr_id, fallback)) return TabReadStatus::TreeUnavailable;
                if (!fallback.empty() || elements.empty()) elements = std::move(fallback);
            }
        }
    }
    redact_sensitive_report_labels(elements);
    return TabReadStatus::Ok;
}

Result ScreenReader::read_with_tabs(bool skip_trees, int max_rows, const std::string& only_tab) {
    TraceGuard trace("ScreenReader::read_with_tabs");
    const Stopwatch timer;
    Result result;

    try {
        if (!session_) {
            result.set_error("NO_SESSION", "No active SAP session");
            return result;
        }

        // A single requested tab never needs the full base read plus every other tab.
        if (!only_tab.empty()) return read_tab(only_tab, skip_trees, max_rows);

        tab_bulk_ = TabBulkStats();
        // Get initial screen structure
        Result initial_result = read(true, skip_trees, max_rows);
        if (initial_result.status != Result::Status::Success) {
            return initial_result;
        }

        json screen_data = initial_result.data;
        json tabs_content = json::array();

        // Find all GuiTab elements in the hierarchy (or under GuiTabStrip)
        std::vector<json> tab_elements = tabs_in_hierarchy(screen_data);

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
            screen_data["hierarchy"]["tabs"], [&](const std::string& strip_id) { return selected_tab_of(strip_id); });
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
            json tab_elements_list;
            (void)expand_tab(tab_elements[i], i, tab_strip_id(tab_elements[i].value("id", "")), skip_trees,
                             current_selected, tabs_content, tabs_failed, tab_elements_list);
        }

        // Only strips whose page actually changed need to be put back.
        if (!restore_tab_selection(selected_tabs, current_selected)) {
            result.set_error("TAB_RESTORE_FAILED", "Screen read could not restore the original tab");
            return result;
        }
        if (!tab_bulk_.unavailable.empty()) return bulk_unavailable_error(tab_bulk_.unavailable);

        if (tabs_content.empty() && !tabs_failed.empty()) {
            const auto& first = tabs_failed.at(0);
            return tab_load_failed(result, "No tab could be read (first failure: " + first.value("tab_id", "") + ", " +
                                               first.value("reason", "error") + ")", tabs_failed);
        }

        // Add tabs content to result
        screen_data["tabs_content"] = tabs_content;
        screen_data["tabs_expanded"] = true;
        screen_data["expanded_tab_count"] = tabs_content.size();
        if (!tabs_failed.empty()) screen_data["tabs_failed"] = tabs_failed;

        result.status = Result::Status::Success;
        result.data = screen_data;
        result.diagnostics = initial_result.diagnostics;
        attach_tab_diagnostics(result, effective_tree_reader_mode());

        result.duration = timer.elapsed();

        spdlog::info("Read screen with {} tabs expanded (duration: {}ms)",
                    tabs_content.size(), result.duration.count());

    } catch (const ComException& e) {
        result.set_error("COM_ERROR", e.what());
        spdlog::error("Screen read with tabs failed: {}", e.what());
    } catch (const std::exception& e) {
        result.set_error("EXCEPTION", e.what());
        spdlog::error("Exception in read_with_tabs: {}", e.what());
    }

    return result;
}

} // namespace sap
} // namespace fairyfly

