// Screen filters of `screen read` (--only, --text, --tab, ...) and the tab coverage notes: pure JSON, no SAP.
#include "include/cli_handler.h"
#include "include/string_utils.h"

#include <algorithm>
#include <map>
#include <set>
#include <unordered_set>
#include <string>

namespace fairyfly {
namespace cli {

/// True when any string value inside `value` contains the (lowercased) needle.
static bool json_strings_contain(const json& value, const std::string& needle, int depth = 0)
{
    if (depth > 64) return false;
    if (value.is_string()) {
        return utils::lower_ascii(value.get<std::string>()).find(needle) != std::string::npos;
    }
    if (value.is_array() || value.is_object()) {
        for (const auto& item : value) {
            if (json_strings_contain(item, needle, depth + 1)) return true;
        }
    }
    return false;
}

/// The element's own text (labels, tooltip, text content), not its table or tree data.
static bool element_own_text_matches(const json& elem, const std::string& needle)
{
    for (const char* key : {"text", "tooltip", "label", "text_content"}) {
        if (elem.contains(key) && json_strings_contain(elem[key], needle)) return true;
    }
    return false;
}

/// Grid / table-control data object that carries a `rows` array, or nullptr.
static json* table_rows_owner(json& elem)
{
    for (const char* key : {"table_data", "grid_data"}) {
        if (elem.contains(key) && elem[key].is_object() && elem[key].contains("rows") &&
            elem[key]["rows"].is_array())
            return &elem[key];
    }
    return nullptr;
}

/// Text search across labels, tooltips, table/grid cells, tree nodes and text content (IMP-005).
static bool element_text_matches(const json& elem, const std::string& needle)
{
    if (element_own_text_matches(elem, needle)) return true;
    for (const char* key : {"table_data", "grid_data", "tree_data", "tree_nodes"}) {
        if (elem.contains(key) && json_strings_contain(elem[key], needle)) return true;
    }
    return false;
}

/// `--text-contains` on a grid or table control keeps only the rows with a matching cell and reports
/// `rows_matched` / `rows_total` (rows read, before filtering). When the element matched through its
/// own text or a column title instead of a cell, its rows stay untouched.
static void narrow_table_rows(json& elem, const std::string& needle)
{
    if (!elem.is_object() || element_own_text_matches(elem, needle)) return;
    json* table = table_rows_owner(elem);
    if (!table) return;
    const json& rows = (*table)["rows"];
    json kept = json::array();
    for (const auto& row : rows) {
        if (json_strings_contain(row, needle)) kept.push_back(row);
    }
    if (kept.empty()) return;  // the element matched elsewhere: its rows stay untouched
    (*table)["rows_total"] = rows.size();
    (*table)["rows_matched"] = kept.size();
    (*table)["rows"] = std::move(kept);
}

/// Grid, table control or positioned-label table (anything rendered as rows and columns).
static bool element_is_table(const json& elem)
{
    if (!elem.is_object()) return false;
    const std::string type = elem.value("type", "");
    if (type == "GuiGridView" || type == "GuiTableControl") return true;
    if (elem.value("subtype", "") == "GridView") return true;
    for (const char* key : {"table_data", "grid_data"}) {
        if (elem.contains(key) && elem[key].is_object()) return true;
    }
    return false;
}

static bool element_is_tree(const json& elem)
{
    if (!elem.is_object()) return false;
    const std::string subtype = elem.value("subtype", "");
    return elem.value("type", "") == "GuiTree" || subtype == "Tree" || subtype == "TableTreeControl" ||
           elem.contains("tree_nodes") || elem.contains("tree_data");
}

/// Helper to check if an element matches filter criteria
static bool element_matches_filter(const json& elem, const ScreenFilterOptions& filters)
{
    if (!elem.is_object()) {
        return false;
    }

    bool matches = true;

    // Type filters
    std::string type = elem.value("type", "");

    if (filters.only_buttons) {
        matches = matches && (type == "GuiButton");
    }

    if (filters.only_fields) {
        matches = matches && (type == "GuiTextField" || type == "GuiCTextField" ||
                             type == "GuiPasswordField") && elem.value("changeable", false);
    }

    if (filters.only_editable) {
        matches = matches && elem.value("changeable", false);
    }

    if (filters.only_tables) {
        matches = matches && element_is_table(elem);
    }

    if (filters.only_f4_fields) {
        matches = matches && elem.value("has_f4_help", false);
    }

    // Text search (case-insensitive)
    if (filters.text_contains.has_value() && matches) {
        matches = element_text_matches(elem, utils::lower_ascii(filters.text_contains.value()));
    }

    // ID search
    if (filters.id_contains.has_value() && matches) {
        std::string id = elem.value("id", "");
        matches = id.find(filters.id_contains.value()) != std::string::npos;
    }

    // Exact type filter
    if (filters.type_filter.has_value() && matches) {
        matches = (type == filters.type_filter.value());
    }

    return matches;
}

/// Recursively filter elements based on ScreenFilterOptions
/// Returns flat array of all matching elements (including nested children)
static std::pair<json, json> filter_elements(const json& elements, const ScreenFilterOptions& filters)
{
    json filter_stats;

    // If no filters active, return all elements
    bool any_filter = filters.only_buttons || filters.only_fields || filters.only_editable ||
                      filters.only_f4_fields || filters.only_tables || filters.text_contains.has_value() ||
                      filters.id_contains.has_value() || filters.type_filter.has_value() ||
                      filters.first_match_only;

    if (!any_filter) {
        filter_stats["filter_applied"] = false;
        return {elements, filter_stats};
    }

    json filtered = json::array();
    int total_checked = 0;
    bool found_first = false;

    // Recursive lambda to collect all matching elements
    std::function<void(const json&)> collect_matches = [&](const json& elem_array) {
        if (!elem_array.is_array()) return;

        for (const auto& elem : elem_array) {
            // Child arrays may mix ID references with fully expanded objects.
            if (!elem.is_object()) continue;
            total_checked++;

            // Check if this element matches
            if (element_matches_filter(elem, filters)) {
                filtered.push_back(elem);
                if (filters.text_contains.has_value())
                    narrow_table_rows(filtered.back(), utils::lower_ascii(filters.text_contains.value()));
                found_first = true;

                // If first_match_only, stop immediately
                if (filters.first_match_only) {
                    return;
                }
            }

            // Recursively check all child objects, even after string ID references.
            if (elem.is_object() && elem.contains("children") && elem["children"].is_array() &&
                (!filters.first_match_only || !found_first)) {
                collect_matches(elem["children"]);
                if (filters.first_match_only && found_first) {
                    return;
                }
            }

            // Also check toolbar_buttons array (synthetic buttons)
            if (elem.is_object() && elem.contains("toolbar_buttons") &&
                elem["toolbar_buttons"].is_array() && (!filters.first_match_only || !found_first)) {
                collect_matches(elem["toolbar_buttons"]);
                if (filters.first_match_only && found_first) {
                    return;
                }
            }
            if (filters.first_match_only && found_first) return;
        }
    };

    collect_matches(elements);

    // Build filter stats
    filter_stats["filter_applied"] = true;
    filter_stats["total_elements_checked"] = total_checked;
    filter_stats["filtered_element_count"] = static_cast<int>(filtered.size());

    return {filtered, filter_stats};
}

/// Name of the `only` selector in effect (MCP argument spelling), empty when none.
static std::string only_selector_name(const ScreenFilterOptions& filters)
{
    if (filters.only_buttons) return "buttons";
    if (filters.only_fields) return "fields";
    if (filters.only_editable) return "editable";
    if (filters.only_f4_fields) return "f4_fields";
    if (filters.only_tables) return "tables";
    return "";
}

/// Grids/trees that an `only` selector dropped: they would otherwise vanish without a trace.
static void collect_suppressed(const json& elements, const std::unordered_set<std::string>& kept,
                               json& suppressed, int depth = 0)
{
    if (!elements.is_array() || depth > 64) return;
    for (const auto& elem : elements) {
        if (!elem.is_object()) continue;
        const std::string id = elem.value("id", "");
        if (kept.count(id) == 0) {
            const char* ids_key = element_is_table(elem) ? "grid_ids" : element_is_tree(elem) ? "tree_ids" : nullptr;
            if (ids_key) {
                auto& ids = suppressed[ids_key];
                if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
            }
        }
        if (elem.contains("children")) collect_suppressed(elem["children"], kept, suppressed, depth + 1);
        if (elem.contains("toolbar_buttons"))
            collect_suppressed(elem["toolbar_buttons"], kept, suppressed, depth + 1);
    }
}

bool screen_filters_need_grid_rows(const ScreenFilterOptions& filters)
{
    return !(filters.only_buttons || filters.only_fields || filters.only_editable || filters.only_f4_fields);
}

static void apply_screen_filters_impl(json& screen_data, const ScreenFilterOptions& filters);

/// Display name of a tab: its caption, else the id's last segment without the "tabp" prefix.
static std::string tab_display_name(const std::string& id, const std::string& text)
{
    if (!text.empty()) return text;
    const auto slash = id.rfind('/');
    std::string last = slash == std::string::npos ? id : id.substr(slash + 1);
    if (last.rfind("tabp", 0) == 0 && last.size() > 4) last = last.substr(4);
    return last;
}

static void collect_tab_headers(const json& node, std::map<std::string, std::string>& tabs, int depth = 0)
{
    if (depth > 64) return;
    if (node.is_array()) {
        for (const auto& item : node) collect_tab_headers(item, tabs, depth + 1);
        return;
    }
    if (!node.is_object()) return;
    if (node.value("type", "") == "GuiTab") {
        const std::string id = node.value("id", "");
        if (!id.empty()) {
            auto& text = tabs[id];
            if (text.empty()) text = node.value("text", "");
        }
    }
    for (const char* key : {"children", "toolbar_buttons"}) {
        if (node.contains(key)) collect_tab_headers(node[key], tabs, depth + 1);
    }
}

json describe_tab_coverage(const json& screen_data)
{
    std::map<std::string, std::string> headers;  // id -> caption, ordered by id (= strip order for tabp ids)
    std::vector<std::string> order;
    const auto remember_order = [&](const json& list) {
        if (!list.is_array()) return;
        for (const auto& item : list)
            if (item.is_object() && item.value("type", "") == "GuiTab") {
                const std::string id = item.value("id", "");
                if (!id.empty() && std::find(order.begin(), order.end(), id) == order.end()) order.push_back(id);
            }
    };
    if (screen_data.contains("elements")) collect_tab_headers(screen_data["elements"], headers);
    if (screen_data.contains("hierarchy")) {
        const auto& hierarchy = screen_data["hierarchy"];
        collect_tab_headers(hierarchy, headers);
        if (hierarchy.is_object())
            for (const auto& [category, list] : hierarchy.items()) remember_order(list);
    }
    if (screen_data.contains("elements")) remember_order(screen_data["elements"]);
    // Strips list their pages by id only in some reads.
    if (screen_data.contains("hierarchy") && screen_data["hierarchy"].is_object() &&
        screen_data["hierarchy"].contains("tabs") && screen_data["hierarchy"]["tabs"].is_array()) {
        for (const auto& entry : screen_data["hierarchy"]["tabs"]) {
            if (!entry.is_object() || entry.value("type", "") != "GuiTabStrip" || !entry.contains("children") ||
                !entry["children"].is_array())
                continue;
            for (const auto& child : entry["children"])
                if (child.is_string()) {
                    headers.emplace(child.get<std::string>(), "");
                    if (std::find(order.begin(), order.end(), child.get<std::string>()) == order.end())
                        order.push_back(child.get<std::string>());
                }
        }
    }
    json searched = json::array();
    std::set<std::string> handled;
    if (screen_data.contains("tabs_content") && screen_data["tabs_content"].is_array()) {
        for (const auto& tab : screen_data["tabs_content"]) {
            if (!tab.is_object()) continue;
            const std::string id = tab.value("tab_id", "");
            searched.push_back(tab_display_name(id, tab.value("tab_name", "")));
            handled.insert(id);
            headers.emplace(id, tab.value("tab_name", ""));
            if (std::find(order.begin(), order.end(), id) == order.end()) order.push_back(id);
        }
    }
    json skipped = json::array();
    if (screen_data.contains("tabs_failed") && screen_data["tabs_failed"].is_array()) {
        for (const auto& tab : screen_data["tabs_failed"]) {
            if (!tab.is_object()) continue;
            const std::string id = tab.value("tab_id", "");
            skipped.push_back({{"tab_id", id}, {"tab_name", tab_display_name(id, tab.value("tab_name", ""))},
                               {"reason", tab.value("reason", "error")}});
            handled.insert(id);
        }
    }
    const bool expanded = screen_data.value("tabs_expanded", false);
    // With --tab only the requested tab is read; without it nothing was expanded (--no-tabs).
    const bool single_tab = expanded && screen_data.contains("tabs_content") && screen_data["tabs_content"].is_array() &&
                            screen_data["tabs_content"].size() < order.size();
    for (const auto& id : order) {
        if (handled.count(id)) continue;
        skipped.push_back({{"tab_id", id}, {"tab_name", tab_display_name(id, headers[id])},
                           {"reason", expanded && single_tab ? "not_requested" : "not_expanded"}});
    }
    if (order.empty() && searched.empty() && skipped.empty()) return json::object();
    return {{"searched", searched}, {"skipped", skipped}};
}

static std::string join_names(const json& names)
{
    std::string out;
    for (const auto& name : names) {
        if (!out.empty()) out += ", ";
        out += name.get<std::string>();
    }
    return out;
}

void annotate_text_filter_coverage(json& screen_data, const json& coverage, size_t matches)
{
    if (!coverage.is_object() || coverage.empty()) return;
    const json& searched = coverage["searched"];
    const json& skipped = coverage["skipped"];
    screen_data["tabs_searched"] = searched;
    if (!skipped.empty()) screen_data["tabs_skipped"] = skipped;

    json not_expanded = json::array();
    json failed = json::array();
    for (const auto& tab : skipped) {
        const std::string reason = tab.value("reason", "");
        (reason == "not_expanded" || reason == "not_requested" ? not_expanded : failed)
            .push_back(tab.value("tab_name", ""));
    }
    if (matches > 0 && skipped.empty()) return;
    std::string note;
    if (matches == 0)
        note = searched.empty() ? "0 matches on the visible screen" : "0 matches in tabs [" + join_names(searched) + "]";
    else
        note = searched.empty() ? "Searched the visible screen" : "Searched tabs [" + join_names(searched) + "]";
    if (!not_expanded.empty()) note += "; tabs not expanded: [" + join_names(not_expanded) + "] (use tab=...)";
    if (!failed.empty()) note += "; tabs that could not be read: [" + join_names(failed) + "]";
    screen_data["text_filter_note"] = note;
}

void apply_screen_filters(json& screen_data, const ScreenFilterOptions& filters)
{
    const json coverage = filters.text_contains.has_value() ? describe_tab_coverage(screen_data) : json::object();
    apply_screen_filters_impl(screen_data, filters);
    if (coverage.empty()) return;
    size_t matches = screen_data.value("element_count", static_cast<size_t>(0));
    if (screen_data.contains("tabs_content") && screen_data["tabs_content"].is_array())
        for (const auto& tab : screen_data["tabs_content"])
            if (tab.is_object()) matches += tab.value("element_count", static_cast<size_t>(0));
    annotate_text_filter_coverage(screen_data, coverage, matches);
}

static void apply_screen_filters_impl(json& screen_data, const ScreenFilterOptions& filters)
{
    json all_elements = json::array();
    if (screen_data.contains("elements") && screen_data["elements"].is_array()) {
        all_elements = screen_data["elements"];
    } else if (screen_data.contains("hierarchy")) {
        const json& hierarchy = screen_data["hierarchy"];
        if (hierarchy.is_array()) {
            all_elements = hierarchy;
        } else if (hierarchy.is_object()) {
            for (const auto& [category, elements] : hierarchy.items()) {
                if (!elements.is_array()) continue;
                for (const auto& element : elements) all_elements.push_back(element);
            }
        }
    }

    auto [filtered, filter_stats] = filter_elements(all_elements, filters);
    if (!filter_stats.value("filter_applied", false)) return;

    const std::string text_needle = filters.text_contains.has_value()
        ? utils::lower_ascii(filters.text_contains.value()) : std::string();
    json categorized = json::object();
    std::unordered_set<std::string> wanted_ids;
    std::unordered_set<std::string> assigned_ids;
    // an element joins its category; with --text-contains a grid keeps only its matching rows
    const auto keep = [&](const std::string& category, const json& element) {
        categorized[category].push_back(element);
        if (!text_needle.empty()) narrow_table_rows(categorized[category].back(), text_needle);
    };
    for (const auto& element : filtered) {
        if (element.is_object()) wanted_ids.insert(element.value("id", ""));
    }
    if (screen_data.contains("hierarchy") && screen_data["hierarchy"].is_object()) {
        for (const auto& [category, elements] : screen_data["hierarchy"].items()) {
            if (!elements.is_array()) continue;
            for (const auto& element : elements) {
                if (!element.is_object()) continue;
                const std::string id = element.value("id", "");
                if (wanted_ids.count(id) && assigned_ids.insert(id).second) keep(category, element);
            }
        }
    }
    // Some synthetic or nested elements have no top-level hierarchy category.
    for (const auto& element : filtered) {
        if (!element.is_object()) continue;
        const std::string id = element.value("id", "");
        if (!assigned_ids.insert(id).second) continue;
        const std::string type = element.value("type", "");
        const char* category = type == "GuiButton" ? "buttons" :
                               type == "GuiTextField" || type == "GuiCTextField" ||
                               type == "GuiPasswordField" || type == "GuiCheckBox" ||
                               type == "GuiComboBox" || type == "GuiRadioButton" ? "form_fields" :
                               type == "GuiTableControl" || type == "GuiGridView" ? "tables" :
                               type == "GuiToolbar" ? "toolbar" : "other";
        keep(category, element);
    }
    screen_data["elements"] = filtered;
    screen_data["element_count"] = filtered.size();
    screen_data["hierarchy"] = categorized;
    screen_data["filter_stats"] = filter_stats;

    const std::string only_name = only_selector_name(filters);
    json suppressed = {{"by", only_name}, {"grid_ids", json::array()}, {"tree_ids", json::array()}};
    if (!only_name.empty()) collect_suppressed(all_elements, wanted_ids, suppressed);

    if (screen_data.contains("tabs_content") && screen_data["tabs_content"].is_array()) {
        for (auto& tab_data : screen_data["tabs_content"]) {
            if (!tab_data.is_object()) continue;
            apply_screen_filters_impl(tab_data, filters);
            if (!tab_data.contains("suppressed")) continue;
            for (const char* key : {"grid_ids", "tree_ids"}) {
                for (const auto& id : tab_data["suppressed"].value(key, json::array())) {
                    auto& ids = suppressed[key];
                    if (std::find(ids.begin(), ids.end(), id) == ids.end()) ids.push_back(id);
                }
            }
        }
    }
    if (!suppressed["grid_ids"].empty() || !suppressed["tree_ids"].empty()) {
        suppressed["grids"] = suppressed["grid_ids"].size();
        suppressed["trees"] = suppressed["tree_ids"].size();
        std::string note;
        const auto describe = [&](const char* key, const char* noun) {
            const size_t count = suppressed[key].size();
            if (count == 0) return;
            if (!note.empty()) note += "; ";
            note += std::to_string(count) + " " + noun + "(s)";
        };
        describe("grid_ids", "grid");
        describe("tree_ids", "tree");
        note += " not included with only=" + only_name +
                (suppressed["grid_ids"].empty() ? "; use no filter" : "; use only=tables or no filter");
        suppressed["note"] = note;
        screen_data["suppressed"] = suppressed;
    }
}

}  // namespace cli
}  // namespace fairyfly
