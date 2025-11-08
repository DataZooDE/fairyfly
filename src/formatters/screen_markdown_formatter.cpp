#include "include/formatters/screen_markdown_formatter.h"
#include "include/grid_analyzer.h"
#include "include/formatters/grid_renderer.h"
#include "include/grid_types.h"
#include "include/formatters/tree_formatter.h"
#include "include/formatters/table_formatter.h"
#include "include/formatters/markdown_table_formatter.h"
#include "include/element_renderer_registry.h"
#include "include/semantic_classifier.h"
#include <spdlog/spdlog.h>
#include <sstream>
#include <map>
#include <vector>
#include <set>
#include <algorithm>
#include <functional>

using json = nlohmann::json;

namespace fairyfly {
namespace cli {

// Forward declarations for helper functions
namespace {
    constexpr int MAX_RECURSION_DEPTH = 50;  // Prevent stack overflow in tree traversal

    std::map<std::string, std::string> build_label_field_map(const json& hierarchy);
    void format_menu(const json& menu, std::ostringstream& oss, const std::string& prefix = "", int level = 0);
    std::string escape_markdown(const std::string& text);
    std::string get_state_icon(bool enabled, bool changeable, const std::string& type);
    std::string format_boolean_state(const std::string& type, bool selected);
    bool is_form_field(const std::string& type);
    void add_field_row(const json& elem, formatters::MarkdownTableFormatter& table,
                      const std::map<std::string, std::string>& label_map);
    void add_button_row(const json& elem, formatters::MarkdownTableFormatter& table);
    void add_form_elements_recursive(const json& element, formatters::MarkdownTableFormatter& table,
                                    const std::map<std::string, std::string>& label_map,
                                    std::set<std::string>& rendered_ids, int depth = 0);
    void format_table_element(std::ostringstream& oss, const json& elem);
}

std::string ScreenMarkdownFormatter::format(const json& data) {
    std::ostringstream oss;

    // Screen title, transaction code, and ID
    std::string title = data.value("title", "");
    std::string transaction = data.value("transaction", "");
    std::string screen_id = data.value("screen_id", "");

    oss << "# ";
    if (!title.empty()) {
        oss << title;
    } else {
        oss << "Screen";
    }
    oss << "\n\n";

    if (!transaction.empty()) {
        oss << "**Transaction:** `" << transaction << "`\n\n";
    }

    oss << "**Screen ID:** `" << screen_id << "`\n\n";

    // Check if we have hierarchy data
    if (!data.contains("hierarchy")) {
        oss << "_No screen structure available_\n";
        return oss.str();
    }

    const auto& hierarchy = data["hierarchy"];

    // GRID LAYOUT DETECTION AND RENDERING
    // Check if this is a grid-based monitoring screen (SMICM, etc.)
    if (data.contains("elements") && GridAnalyzer::detect_grid_layout(data["elements"])) {
        spdlog::debug("Grid layout detected, using grid renderer");

        // Parse grid labels into cells
        auto grid_cells = GridAnalyzer::parse_grid_labels(data["elements"]);
        spdlog::debug("Parsed {} grid cells", grid_cells.size());

        // Deduplicate cells by ID (some elements may appear in multiple places in the tree)
        auto deduped_cells = GridAnalyzer::deduplicate_cells(grid_cells);
        spdlog::debug("Deduplicated {} cells", deduped_cells.size());

        // Identify sections (status pairs and rotated tables)
        auto sections = GridAnalyzer::identify_grid_sections(deduped_cells);
        spdlog::debug("Identified {} grid sections", sections.size());

        // Render each section
        for (const auto& section : sections) {
            if (section.type == GridSection::StatusPairs) {
                oss << GridRenderer::render_status_section(section.cells);
            } else if (section.type == GridSection::RotatedTable) {
                oss << GridRenderer::render_rotated_table(section.cells);
            }
        }

        // Add menu bar if present (monitoring screens still have menus)
        if (hierarchy.contains("other") && !hierarchy["other"].empty()) {
            std::vector<json> menu_items;
            for (const auto& elem : hierarchy["other"]) {
                std::string type = elem.value("type", "");
                std::string id = elem.value("id", "");
                if (type == "GuiMenu" && id.find("/mbar/menu[") != std::string::npos) {
                    size_t mbar_pos = id.find("/mbar/menu[");
                    size_t after_menu = id.find("]", mbar_pos + 11);
                    if (after_menu != std::string::npos && after_menu + 1 == id.length()) {
                        menu_items.push_back(elem);
                    }
                }
            }

            if (!menu_items.empty()) {
                oss << "## Menu Bar\n\n";
                for (const auto& menu : menu_items) {
                    format_menu(menu, oss);
                }
                oss << "\n";
            }
        }

        // Add buttons if present
        if (hierarchy.contains("buttons") && !hierarchy["buttons"].empty()) {
            oss << "## Buttons\n\n";
            for (const auto& elem : hierarchy["buttons"]) {
                std::string label = elem.value("text", "");
                if (label.empty()) {
                    label = elem.value("name", "Button");
                }
                std::string id = elem.value("id", "");
                bool enabled = elem.value("enabled", true);

                oss << "- **" << escape_markdown(label) << "**";
                if (!enabled) {
                    oss << " _(disabled)_";
                }
                oss << " — `" << id << "`\n";
            }
            oss << "\n";
        }

        // IMPORTANT: Also render non-grid elements for report content
        // Report logs use individual GuiLabel elements with grid positions
        // Collect labels by row and format them together
        if (hierarchy.contains("other") && !hierarchy["other"].empty()) {
            struct LabelInfo {
                std::string text;
                int row;
                int col;
            };
            std::vector<LabelInfo> labels;

            std::function<void(const json&, int)> collect_labels = [&](const json& elem, int depth) {
                if (depth > MAX_RECURSION_DEPTH) {
                    spdlog::warn("Max recursion depth reached in collect_labels");
                    return;
                }

                std::string type = elem.value("type", "");

                // Collect text from GuiLabel elements with grid positions
                if (type == "GuiLabel") {
                    std::string text = elem.value("text", "");
                    // Skip whitespace-only labels
                    std::string trimmed = text;
                    trimmed.erase(0, trimmed.find_first_not_of(" \t\n\r"));
                    trimmed.erase(trimmed.find_last_not_of(" \t\n\r") + 1);

                    if (!trimmed.empty() && trimmed.length() > 2) {
                        LabelInfo info;
                        info.text = trimmed;
                        info.row = elem.value("grid_row", -1);
                        info.col = elem.value("grid_col", -1);
                        labels.push_back(info);
                    }
                }

                // Recurse into children
                if (elem.contains("children") && elem["children"].is_array()) {
                    for (const auto& child : elem["children"]) {
                        collect_labels(child, depth + 1);
                    }
                }
            };

            // Collect all labels
            for (const auto& elem : hierarchy["other"]) {
                collect_labels(elem, 0);
            }

            // Group labels by row and format them
            if (!labels.empty()) {
                std::map<int, std::vector<LabelInfo>> rows;
                for (const auto& label : labels) {
                    if (label.row >= 0) {
                        rows[label.row].push_back(label);
                    }
                }

                // Sort each row by column
                for (auto& [row, row_labels] : rows) {
                    std::sort(row_labels.begin(), row_labels.end(),
                             [](const LabelInfo& a, const LabelInfo& b) { return a.col < b.col; });
                }

                // Render row by row
                oss << "## Report Output\n\n";
                oss << "```\n";
                for (const auto& [row, row_labels] : rows) {
                    // Combine labels in the same row
                    std::string line;
                    for (size_t i = 0; i < row_labels.size(); ++i) {
                        if (i > 0) line += " ";
                        line += row_labels[i].text;
                    }
                    if (!line.empty()) {
                        oss << line << "\n";
                    }
                }
                oss << "```\n\n";
            }
        }

        // Summary stats
        int total_elements = data.value("element_count", 0);
        oss << "---\n";
        oss << "**Screen Elements:** " << total_elements << " top-level\n";

        return oss.str();
    }
    // END GRID LAYOUT RENDERING

    // Build label-to-field mapping
    auto label_map = build_label_field_map(hierarchy);

    // Menu bar section - extract from "other" category
    if (hierarchy.contains("other") && !hierarchy["other"].empty()) {
        // Find the menu bar (GuiMenubar) and extract top-level menus
        std::vector<json> menu_items;
        for (const auto& elem : hierarchy["other"]) {
            std::string type = elem.value("type", "");
            std::string id = elem.value("id", "");
            // Top-level menus have pattern: /wnd[0]/mbar/menu[N]
            if (type == "GuiMenu" && id.find("/mbar/menu[") != std::string::npos) {
                // Check if this is a direct child of mbar (top-level menu)
                size_t mbar_pos = id.find("/mbar/menu[");
                size_t after_menu = id.find("]", mbar_pos + 11);
                if (after_menu != std::string::npos && after_menu + 1 == id.length()) {
                    menu_items.push_back(elem);
                }
            }
        }

        if (!menu_items.empty()) {
            oss << "## Menu Bar\n\n";
            for (const auto& menu : menu_items) {
                format_menu(menu, oss);
            }
        }
    }

    // Buttons section (toolbar and action buttons combined)
    // Collect buttons from both hierarchy["buttons"] AND GuiShell toolbar children
    std::vector<json> all_buttons;

    // Add buttons from hierarchy["buttons"] (traditional SAP GUI toolbar)
    if (hierarchy.contains("buttons") && !hierarchy["buttons"].empty()) {
        for (const auto& elem : hierarchy["buttons"]) {
            all_buttons.push_back(elem);
        }
    }

    // Add buttons from GuiShell toolbars (hierarchy["toolbar"] with SubType="Toolbar")
    if (hierarchy.contains("toolbar") && !hierarchy["toolbar"].empty()) {
        for (const auto& toolbar_elem : hierarchy["toolbar"]) {
            std::string type = toolbar_elem.value("type", "");
            std::string subtype = toolbar_elem.value("subtype", "");

            // Check if this is a GuiShell Toolbar with synthetic button metadata
            if (type == "GuiShell" && subtype == "Toolbar" &&
                toolbar_elem.contains("toolbar_buttons") && toolbar_elem["toolbar_buttons"].is_array()) {
                for (const auto& button : toolbar_elem["toolbar_buttons"]) {
                    all_buttons.push_back(button);
                }
            }
        }
    }

    // Render buttons table if we have any buttons
    if (!all_buttons.empty()) {
        oss << "## Toolbar Buttons\n\n";

        formatters::MarkdownTableFormatter table;
        table.add_column("Button", formatters::MarkdownTableFormatter::Alignment::Left);
        table.add_column("Tooltip", formatters::MarkdownTableFormatter::Alignment::Left);
        table.add_column("State", formatters::MarkdownTableFormatter::Alignment::Left);
        table.add_column("Technical ID", formatters::MarkdownTableFormatter::Alignment::Left);

        for (const auto& elem : all_buttons) {
            std::string label = elem.value("text", "");
            if (label.empty()) {
                label = elem.value("name", "Button");
            }
            std::string tooltip = elem.value("tooltip", "");
            std::string id = elem.value("id", "");
            bool enabled = elem.value("enabled", true);
            std::string state = enabled ? "Enabled" : "Disabled";

            std::vector<std::string> cells = {label, tooltip, state, id};
            std::vector<formatters::MarkdownTableFormatter::CellStyle> styles(4);
            styles[0].bold = true;  // Button name bold
            styles[3].code = true;  // ID in code style

            table.add_row(cells, styles);
        }

        oss << table.render() << "\n";
        oss << "_Click with_: `fairyfly click '@active/<button_path>'`\n\n";
    }

    // Form fields and content section - walk DOM tree to maintain spatial layout
    if (data.contains("elements") && data["elements"].is_array() && !data["elements"].empty()) {
        // Check if we have any form fields or inline buttons
        bool has_form_content = hierarchy.contains("form_fields") && !hierarchy["form_fields"].empty();
        bool has_inline_buttons = hierarchy.contains("inline_buttons") && !hierarchy["inline_buttons"].empty();

        if (has_form_content || has_inline_buttons) {
            // Create table formatter
            formatters::MarkdownTableFormatter table;
            table.add_column("Field", formatters::MarkdownTableFormatter::Alignment::Left);
            table.add_column("Value", formatters::MarkdownTableFormatter::Alignment::Left);
            table.add_column("State", formatters::MarkdownTableFormatter::Alignment::Left);
            table.add_column("Technical ID", formatters::MarkdownTableFormatter::Alignment::Left);

            // Walk the element tree to add rows in DOM order
            std::set<std::string> rendered_ids;  // Track rendered elements to avoid duplicates
            for (const auto& root_elem : data["elements"]) {
                add_form_elements_recursive(root_elem, table, label_map, rendered_ids);
            }

            // Render the table
            oss << "## Form Fields\n\n";
            oss << table.render();
            oss << "\n";
        }
    }

    // Tabs section with expanded content support
    bool tabs_expanded = data.value("tabs_expanded", false);

    if (tabs_expanded && data.contains("tabs_content") && !data["tabs_content"].empty()) {
        // Display expanded tabs with full content
        oss << "## Tabs (Expanded)\n\n";

        for (const auto& tab_data : data["tabs_content"]) {
            std::string tab_name = tab_data.value("tab_name", "Unnamed Tab");
            std::string tab_id = tab_data.value("tab_id", "");
            int elem_count = tab_data.value("element_count", 0);

            oss << "### " << escape_markdown(tab_name) << "\n";
            oss << "**Technical ID:** `" << tab_id << "` | **Elements:** " << elem_count << "\n\n";

            // Extract and display tab-specific fields
            if (tab_data.contains("hierarchy") && tab_data["hierarchy"].contains("form_fields")) {
                const auto& fields = tab_data["hierarchy"]["form_fields"];
                auto tab_label_map = build_label_field_map(tab_data["hierarchy"]);

                if (!fields.empty()) {
                    oss << "| Field | Value | State | Technical ID |\n";
                    oss << "|-------|-------|-------|-------------|\n";

                    for (const auto& elem : fields) {
                        std::string type = elem.value("type", "");
                        std::string id = elem.value("id", "");
                        std::string text = elem.value("text", "");
                        std::string name = elem.value("name", "");

                        // Skip labels (they're paired with fields)
                        if (type == "GuiLabel") continue;

                        // Format value based on type
                        std::string value_str = escape_markdown(text);
                        bool is_empty = value_str.empty();
                        if (is_empty) {
                            value_str = "_empty_";
                        }

                        // Skip empty rows to reduce clutter (only for table content, not form fields)
                        // Check if this looks like a table row (has row indices in the path like [0,n])
                        bool is_table_row = id.find("[0,") != std::string::npos ||
                                           id.find("[1,") != std::string::npos;
                        if (is_table_row && is_empty) {
                            continue;  // Skip empty table cells
                        }

                        // Get label text for this field
                        std::string label_text = "";
                        if (tab_label_map.count(id)) {
                            label_text = tab_label_map[id];
                        } else if (!name.empty()) {
                            label_text = name;
                        }

                        // Get state icon
                        bool enabled = elem.value("enabled", true);
                        bool changeable = elem.value("changeable", false);
                        std::string state_icon = get_state_icon(enabled, changeable, type);

                        // Build row
                        oss << "| ";
                        if (!label_text.empty()) {
                            oss << "**" << escape_markdown(label_text) << "**";
                        } else {
                            oss << escape_markdown(name);
                        }
                        oss << " | " << value_str;
                        oss << " | " << state_icon;
                        oss << " | `" << id << "` |\n";
                    }
                    oss << "\n";
                }
            }

            // Display source code if tab contains an AbapEditor element with text content
            if (tab_data.contains("hierarchy") && tab_data["hierarchy"].contains("elements")) {
                const auto& elements = tab_data["hierarchy"]["elements"];

                for (const auto& elem : elements) {
                    std::string type = elem.value("type", "");
                    std::string subtype = elem.value("subtype", "");

                    // Check if this is a GuiShell with AbapEditor subtype and text content
                    if (type == "GuiShell" && subtype == "AbapEditor" && elem.contains("text_content")) {
                        std::string text_content = elem.value("text_content", "");

                        if (!text_content.empty() && text_content != "null") {
                            // Render as ABAP code block
                            oss << "```abap\n";
                            oss << escape_markdown(text_content);
                            oss << "\n```\n\n";
                            break;  // Only one editor per tab
                        }
                    }
                }
            }

            // Display buttons if any
            if (tab_data.contains("hierarchy") && tab_data["hierarchy"].contains("buttons")) {
                const auto& buttons = tab_data["hierarchy"]["buttons"];
                if (!buttons.empty()) {
                    oss << "**Buttons:** ";
                    bool first = true;
                    for (const auto& btn : buttons) {
                        if (!first) oss << " | ";
                        first = false;

                        std::string label = btn.value("text", btn.value("name", ""));
                        std::string id = btn.value("id", "");
                        oss << escape_markdown(label) << " `" << id << "`";
                    }
                    oss << "\n\n";
                }
            }
        }
    } else if (hierarchy.contains("tabs") && !hierarchy["tabs"].empty()) {
        // Display non-expanded tabs summary
        oss << "## 📑 Tabs\n\n";

        // Try to find the TabStrip container
        std::string tabstrip_id = "";
        for (const auto& elem : hierarchy["tabs"]) {
            std::string id = elem.value("id", "");
            if (id.find("tabs") != std::string::npos) {
                // Extract parent TabStrip ID
                size_t pos = id.find("/tabp");
                if (pos != std::string::npos) {
                    tabstrip_id = id.substr(0, pos);
                    break;
                }
            }
        }

        if (!tabstrip_id.empty()) {
            oss << "**Tab Container:** `" << tabstrip_id << "`\n\n";
        }

        oss << "| Tab Name | Status | Technical ID |\n";
        oss << "|----------|--------|-------------|\n";

        for (const auto& elem : hierarchy["tabs"]) {
            std::string label = elem.value("text", "");
            if (label.empty()) {
                label = elem.value("name", "Tab");
            }
            std::string id = elem.value("id", "");

            // Assume first tab or tab with type "GuiTabStrip" is active
            // In reality, would need to check Selected property
            bool is_active = false;  // Would need additional metadata

            oss << "| " << (is_active ? "**" : "") << escape_markdown(label) << (is_active ? "**" : "") << " | ";
            oss << (is_active ? "▶️ Active" : "Available") << " | ";
            oss << "`" << id << "` |\n";
        }
        oss << "\n";

        oss << "_Note: Tab expansion is enabled by default. Use `--no-tabs` flag to skip tab content capture for faster execution._\n\n";
    }

    // Tables and Grids section - use new formatters
    if (hierarchy.contains("tables") && !hierarchy["tables"].empty()) {
        oss << "## Tables and Grids\n\n";

        formatters::TableFormatter table_formatter;
        for (const auto& elem : hierarchy["tables"]) {
            std::string type = elem.value("type", "");
            std::string subtype = elem.value("subtype", "");

            // Use TableFormatter for GuiGridView, GuiTableControl, or GuiShell with GridView subtype
            if (table_formatter.can_format(type) ||
                (type == "GuiShell" && subtype == "GridView") ||
                elem.contains("table_data")) {
                // Use new formatter with table_data support
                table_formatter.format_to_markdown(elem, oss);
            } else {
                // Fallback to old formatting
                format_table_element(oss, elem);
            }
        }
    }

    // Additional screen content (text editors, trees, etc.) shown inline
    if (hierarchy.contains("other") && !hierarchy["other"].empty()) {
        auto& registry = sap::ElementRendererRegistry::instance();

        // Build set of all element IDs to identify parent-child relationships
        std::set<std::string> all_ids;
        std::set<std::string> child_ids;

        for (const auto& elem : hierarchy["other"]) {
            std::string id = elem.value("id", "");
            if (!id.empty()) {
                all_ids.insert(id);
            }
        }

        // Collect all IDs that appear as children of other elements
        for (const auto& elem : hierarchy["other"]) {
            if (elem.contains("children") && elem["children"].is_array()) {
                for (const auto& child : elem["children"]) {
                    std::string child_id = child.value("id", "");
                    if (!child_id.empty()) {
                        child_ids.insert(child_id);
                    }
                }
            }
        }

        // Recursively find and render semantic elements
        std::function<void(const json&, int)> render_semantic_descendants = [&](const json& elem, int depth) {
            if (depth > MAX_RECURSION_DEPTH) {
                spdlog::warn("Max recursion depth reached in render_semantic_descendants");
                return;
            }

            std::string type = elem.value("type", "");
            std::string subtype = elem.value("subtype", "");

            // Check if this element is truly semantic (has meaningful content)
            bool is_truly_semantic = (subtype == "TextEdit") ||
                                    (subtype == "Tree" || elem.contains("tree_data")) ||
                                    (subtype == "GridView" || elem.contains("table_data"));

            if (is_truly_semantic && sap::SemanticClassifier::should_display(elem)) {
                std::string rendered = registry.render_to_markdown(elem, 0);
                if (!rendered.empty()) {
                    oss << rendered;
                }
                return; // Don't recurse into children if we rendered this element
            }

            // Otherwise, recurse into children
            if (elem.contains("children") && elem["children"].is_array()) {
                for (const auto& child : elem["children"]) {
                    render_semantic_descendants(child, depth + 1);
                }
            }
        };

        // Start from top-level elements only
        for (const auto& elem : hierarchy["other"]) {
            std::string id = elem.value("id", "");
            bool is_top_level = (id.empty() || child_ids.find(id) == child_ids.end());

            if (is_top_level) {
                render_semantic_descendants(elem, 0);
            }
        }
    }

    // Summary stats
    int total_elements = data.value("element_count", 0);
    oss << "---\n";
    oss << "**Screen Elements:** " << total_elements << " top-level\n";

    return oss.str();
}

// Helper function implementations
namespace {

std::map<std::string, std::string> build_label_field_map(const json& hierarchy) {
    std::map<std::string, std::string> label_map; // field_id -> label_text

    if (!hierarchy.contains("form_fields")) {
        return label_map;
    }

    const auto& fields = hierarchy["form_fields"];
    std::vector<json> labels;
    std::vector<json> inputs;

    // Separate labels from input fields
    for (const auto& elem : fields) {
        std::string type = elem.value("type", "");
        if (type == "GuiLabel") {
            labels.push_back(elem);
        } else if (type.find("TextField") != std::string::npos ||
                   type.find("ComboBox") != std::string::npos ||
                   type.find("CheckBox") != std::string::npos ||
                   type.find("RadioButton") != std::string::npos) {
            inputs.push_back(elem);
        }
    }

    // Match labels to fields by name pattern
    for (const auto& input : inputs) {
        std::string input_name = input.value("name", "");
        std::string input_id = input.value("id", "");

        for (const auto& label : labels) {
            std::string label_name = label.value("name", "");
            std::string label_text = label.value("text", "");

            // Remove "lbl" prefix from label name to match field name
            std::string label_base = label_name;
            if (label_base.find("lbl") == 0) {
                label_base = label_base.substr(3);
            }

            // Check if names match
            if (input_name.find(label_base) != std::string::npos ||
                label_base.find(input_name) != std::string::npos) {
                label_map[input_id] = label_text;
                break;
            }
        }
    }

    return label_map;
}

void format_menu(const json& menu, std::ostringstream& oss, const std::string& /* prefix */, int level) {
    if (level > MAX_RECURSION_DEPTH) {
        spdlog::warn("Max recursion depth reached in format_menu");
        return;
    }

    std::string text = menu.value("text", menu.value("name", ""));
    if (text.empty()) return;  // Skip empty menu items (separators)

    auto children = menu.value("children", json::array());

    // Skip System and Help menus at top level (always the same)
    if (level == 0 && (text == "System" || text == "Help")) {
        return;
    }

    // Format this menu item
    if (level == 0) {
        // Top-level menu: show items inline
        if (!children.empty()) {
            oss << "- **" << escape_markdown(text) << "**: ";
            // Show first-level children inline
            std::vector<std::string> items;
            for (const auto& child : children) {
                std::string child_text = child.value("text", child.value("name", ""));
                if (!child_text.empty()) {
                    items.push_back(child_text);
                }
            }
            for (size_t i = 0; i < items.size(); ++i) {
                oss << items[i];
                if (i < items.size() - 1) oss << ", ";
            }
            oss << "\n";
        } else {
            oss << "- **" << escape_markdown(text) << "**\n";
        }
    }
    // Skip deeper levels in compact mode
}

std::string escape_markdown(const std::string& text) {
    std::string result = text;
    // Replace pipe characters that would break tables
    size_t pos = 0;
    while ((pos = result.find("|", pos)) != std::string::npos) {
        result.replace(pos, 1, "\\|");
        pos += 2;
    }
    return result;
}

std::string get_state_icon(bool enabled, bool changeable, const std::string& /* type */) {
    if (!enabled) {
        return "Disabled";
    } else if (!changeable) {
        return "Read-only";
    } else {
        return "Editable";
    }
}

std::string format_boolean_state(const std::string& type, bool selected) {
    if (type.find("CheckBox") != std::string::npos) {
        return selected ? "[X]" : "[ ]";
    } else if (type.find("RadioButton") != std::string::npos) {
        return selected ? "(•)" : "( )";
    }
    return "";
}

bool is_form_field(const std::string& type) {
    return type == "GuiTextField" || type == "GuiCTextField" ||
           type == "GuiPasswordField" || type == "GuiComboBox" ||
           type == "GuiCheckBox" || type == "GuiRadioButton" ||
           type == "GuiLabel";
}

void add_field_row(const json& elem, formatters::MarkdownTableFormatter& table,
                  const std::map<std::string, std::string>& label_map) {
    std::string type = elem.value("type", "");

    // Skip labels - they're used for semantic context only
    if (type == "GuiLabel") {
        return;
    }

    std::string id = elem.value("id", "");
    std::string text = elem.value("text", "");
    std::string tooltip = elem.value("tooltip", "");
    bool changeable = elem.value("changeable", false);
    bool enabled = elem.value("enabled", true);
    bool selected = elem.value("selected", false);

    // Get label for this field
    std::string label = "";
    auto it = label_map.find(id);
    if (it != label_map.end()) {
        label = it->second;
    }

    // For radio buttons and checkboxes, the label is often in the text property
    if (label.empty() && (type.find("RadioButton") != std::string::npos ||
                          type.find("CheckBox") != std::string::npos)) {
        label = elem.value("text", "");
    }

    if (label.empty()) {
        label = elem.value("name", "Field");
    }

    // Field name with optional tooltip as description
    std::string field_col = label;
    if (!tooltip.empty()) {
        field_col += "\n" + tooltip;  // Will be converted to <br> by formatter
    }

    // Value column
    std::string value_col;
    if (type.find("CheckBox") != std::string::npos || type.find("RadioButton") != std::string::npos) {
        value_col = format_boolean_state(type, selected);
    } else if (type.find("Password") != std::string::npos) {
        value_col = "●●●●●●●●";
    } else if (text.empty()) {
        value_col = "(empty)";
    } else {
        value_col = text;
    }

    // State column
    std::string state_col = get_state_icon(enabled, changeable, type);

    // Technical ID column
    std::string id_col = id;

    // Add row with styles
    std::vector<std::string> cells = {field_col, value_col, state_col, id_col};
    std::vector<formatters::MarkdownTableFormatter::CellStyle> styles(4);
    styles[0].bold = true;  // Field name in bold
    styles[3].code = true;  // Technical ID in code

    table.add_row(cells, styles);
}

void add_button_row(const json& elem, formatters::MarkdownTableFormatter& table) {
    std::string label = elem.value("text", "");
    if (label.empty()) {
        label = elem.value("name", "Button");
    }

    std::string id = elem.value("id", "");
    bool enabled = elem.value("enabled", true);

    // Button as table row with special formatting
    std::string field_col = "[Button: " + label + "]";
    std::string value_col = "-";
    std::string state_col = enabled ? "Enabled" : "Disabled";
    std::string id_col = id;

    std::vector<std::string> cells = {field_col, value_col, state_col, id_col};
    std::vector<formatters::MarkdownTableFormatter::CellStyle> styles(4);
    styles[0].bold = true;  // Button label in bold
    styles[3].code = true;  // Technical ID in code

    table.add_row(cells, styles);
}

void add_form_elements_recursive(const json& element, formatters::MarkdownTableFormatter& table,
                                const std::map<std::string, std::string>& label_map,
                                std::set<std::string>& rendered_ids, int depth) {
    if (depth > MAX_RECURSION_DEPTH) {
        spdlog::warn("Max recursion depth reached in add_form_elements_recursive");
        return;
    }

    if (!element.is_object()) {
        return;
    }

    std::string type = element.value("type", "");
    std::string id = element.value("id", "");

    // Skip if already rendered (deduplication)
    if (!id.empty() && !rendered_ids.insert(id).second) {
        return;
    }

    // Render GuiBox as section header (visual grouping)
    if (type == "GuiBox") {
        std::string name = element.value("name", "");
        if (!name.empty()) {
            table.add_section_header(name);
        }
    }

    // Render form fields as table rows
    if (is_form_field(type)) {
        add_field_row(element, table, label_map);
    }

    // Render inline buttons (in /usr/) as special table rows
    if (type == "GuiButton" && id.find("/usr/") != std::string::npos) {
        add_button_row(element, table);
    }

    // Recurse into children
    if (element.contains("children") && element["children"].is_array()) {
        for (const auto& child : element["children"]) {
            add_form_elements_recursive(child, table, label_map, rendered_ids, depth + 1);
        }
    }
}

void format_table_element(std::ostringstream& oss, const json& elem) {
    std::string type = elem.value("type", "");
    std::string name = elem.value("name", "");
    std::string id = elem.value("id", "");
    int child_count = elem.value("child_count", 0);
    int row_count = elem.value("row_count", 0);
    int visible_rows = elem.value("visible_rows", 0);

    oss << "### " << (name.empty() ? "Table" : escape_markdown(name)) << "\n\n";
    oss << "| Property | Value |\n";
    oss << "|----------|-------|\n";
    oss << "| **Technical ID** | `" << id << "` |\n";
    oss << "| **Type** | " << type << " |\n";

    if (row_count > 0) {
        oss << "| **Total Rows** | " << row_count << " |\n";
    }
    if (visible_rows > 0) {
        oss << "| **Visible Rows** | " << visible_rows << " |\n";
    }
    if (child_count > 0) {
        oss << "| **Elements** | " << child_count << " |\n";
    }
    oss << "\n";

    // Add usage examples based on type
    if (type == "GuiGridView") {
        oss << "**Usage Examples:**\n";
        oss << "```python\n";
        oss << "# Read cell value\n";
        oss << "fairyfly get '" << id << "' --row 0 --column 'COLUMN_NAME'\n\n";
        oss << "# Set cell value\n";
        oss << "fairyfly fill '" << id << "' 'new_value' --row 0 --column 'COLUMN_NAME'\n\n";
        oss << "# Click cell to select row\n";
        oss << "fairyfly click '" << id << "' --row 0 --column 'COLUMN_NAME'\n";
        oss << "```\n\n";
    } else if (type == "GuiTableControl") {
        oss << "**Usage Examples:**\n";
        oss << "```python\n";
        oss << "# Access cell via element path\n";
        oss << "fairyfly get '" << id << "/txt[row,col]'\n\n";
        oss << "# Navigate to row\n";
        oss << "fairyfly click '" << id << "' --row 0\n";
        oss << "```\n\n";
    }
}

} // anonymous namespace

} // namespace cli
} // namespace fairyfly

