#include "include/semantic_classifier.h"
#include <algorithm>

namespace fairyfly {
namespace sap {

SemanticClassifier::ElementCategory SemanticClassifier::classify(
    const std::string& type,
    const json& metadata
) {
    // Semantic elements - always display
    if (type == "GuiTree" ||
        type == "GuiGridView" ||
        type == "GuiTableControl" ||
        type == "GuiButton" ||
        type == "GuiTextField" ||
        type == "GuiCTextField" ||
        type == "GuiComboBox" ||
        type == "GuiCheckBox" ||
        type == "GuiRadioButton" ||
        type == "GuiLabel" ||
        type == "GuiTab" ||
        type == "GuiTabStrip") {
        return ElementCategory::SEMANTIC;
    }

    // GuiShell can be semantic (toolbar, tree, grid, textedit) or layout depending on SubType/content
    if (type == "GuiShell") {
        std::string subtype = metadata.value("subtype", "");
        if (subtype == "Tree" || subtype == "Toolbar" || subtype == "GridView" || subtype == "TextEdit") {
            return ElementCategory::SEMANTIC;
        }
        // Check if it has tree_data or table_data
        if (metadata.contains("tree_data") && !metadata["tree_data"].is_null()) {
            return ElementCategory::SEMANTIC;
        }
        if (metadata.contains("table_data") && !metadata["table_data"].is_null()) {
            return ElementCategory::SEMANTIC;
        }
        return ElementCategory::LAYOUT;
    }

    // Splitters are structural markers
    if (type == "GuiSplitterShell") {
        return ElementCategory::STRUCTURAL;
    }

    // Pure layout containers
    if (type == "GuiContainerShell" ||
        type == "GuiCustomControl" ||
        type == "GuiSimpleContainer" ||
        type == "GuiScrollContainer" ||
        type == "GuiDockShell") {
        return ElementCategory::LAYOUT;
    }

    // Frame/Box are structural if they group content
    if (type == "GuiBox" || type == "GuiFrame") {
        return ElementCategory::STRUCTURAL;
    }

    // Hide infrastructure elements (menus handled separately, status bars not semantically useful)
    if (type == "GuiMenu" ||
        type == "GuiMenubar" ||
        type == "GuiOkCodeField" ||
        type == "GuiTitlebar" ||
        type == "GuiUserArea" ||
        type == "GuiStatusbar" ||
        type == "GuiStatusPane") {
        return ElementCategory::LAYOUT;
    }

    // Default: treat as semantic to be safe
    return ElementCategory::SEMANTIC;
}

bool SemanticClassifier::should_display(const json& elem_json) {
    std::string type = elem_json.value("type", "");
    ElementCategory category = classify(type, elem_json);

    // Always show semantic elements
    if (category == ElementCategory::SEMANTIC) {
        return true;
    }

    // Show structural elements (splitters) only if they have semantic children
    if (category == ElementCategory::STRUCTURAL) {
        return has_semantic_children(elem_json);
    }

    // For layout elements, only show if they have semantic children
    if (category == ElementCategory::LAYOUT) {
        return has_semantic_children(elem_json);
    }

    return false;
}

bool SemanticClassifier::is_layout_only(const std::string& type) {
    return type == "GuiContainerShell" ||
           type == "GuiCustomControl" ||
           type == "GuiSimpleContainer" ||
           type == "GuiScrollContainer" ||
           type == "GuiDockShell" ||
           type == "GuiUserArea";
}

bool SemanticClassifier::has_semantic_children(const json& elem_json) {
    if (!elem_json.contains("children") || !elem_json["children"].is_array()) {
        return false;
    }

    const auto& children = elem_json["children"];
    for (const auto& child : children) {
        std::string child_type = child.value("type", "");
        ElementCategory child_category = classify(child_type, child);

        if (child_category == ElementCategory::SEMANTIC) {
            return true;
        }

        // Recursively check if child has semantic descendants
        if (has_semantic_children(child)) {
            return true;
        }
    }

    return false;
}

std::string SemanticClassifier::get_semantic_description(
    const std::string& type,
    const json& metadata
) {
    // Tree controls
    if (type == "GuiTree" || (type == "GuiShell" && metadata.contains("tree_data"))) {
        if (metadata.contains("tree_data")) {
            const auto& tree_data = metadata["tree_data"];
            if (tree_data.contains("nodes") && tree_data["nodes"].is_array()) {
                // Count total nodes recursively
                std::function<int(const json&)> count_nodes = [&](const json& nodes) -> int {
                    int count = static_cast<int>(nodes.size());
                    for (const auto& node : nodes) {
                        if (node.contains("children") && node["children"].is_array()) {
                            count += count_nodes(node["children"]);
                        }
                    }
                    return count;
                };
                int node_count = count_nodes(tree_data["nodes"]);
                return "Tree (" + std::to_string(node_count) + " nodes)";
            }
        }
        return "Tree";
    }

    // GuiShell with specific SubTypes
    if (type == "GuiShell") {
        std::string subtype = metadata.value("subtype", "");

        // GridView subtype - treat as table/grid
        if (subtype == "GridView") {
            if (metadata.contains("table_data")) {
                const auto& table_data = metadata["table_data"];
                int row_count = table_data.value("row_count", 0);
                int col_count = table_data.value("column_count", 0);
                return "GridView (" + std::to_string(row_count) + " rows × " +
                       std::to_string(col_count) + " cols)";
            }
            return "GridView";
        }

        // Toolbar subtype
        if (subtype == "Toolbar") {
            return "Toolbar";
        }

        // TextEdit subtype
        if (subtype == "TextEdit") {
            std::string text = metadata.value("text", "");
            int line_count = 1;
            for (char c : text) {
                if (c == '\n') line_count++;
            }
            if (line_count > 1) {
                return "Text Editor (" + std::to_string(line_count) + " lines)";
            }
            return "Text Editor";
        }

        // Fallback: check text content
        std::string text = metadata.value("text", "");
        if (text.find("Toolbar") != std::string::npos) {
            return "Toolbar";
        }
    }

    // Tables
    if (type == "GuiGridView" || type == "GuiTableControl") {
        if (metadata.contains("table_data")) {
            const auto& table_data = metadata["table_data"];
            int row_count = table_data.value("row_count", 0);
            int col_count = table_data.value("column_count", 0);
            return "Table (" + std::to_string(row_count) + " rows, " +
                   std::to_string(col_count) + " cols)";
        }
        return "Table";
    }

    // Splitter
    if (type == "GuiSplitterShell") {
        int panel_count = 0;
        if (metadata.contains("children") && metadata["children"].is_array()) {
            panel_count = static_cast<int>(metadata["children"].size());
        }
        return "Splitter (" + std::to_string(panel_count) + " panels)";
    }

    // Buttons
    if (type == "GuiButton") {
        std::string name = metadata.value("name", "");
        std::string text = metadata.value("text", "");
        if (!text.empty()) {
            return "Button: " + text;
        }
        return "Button";
    }

    // Text fields
    if (type == "GuiTextField" || type == "GuiCTextField") {
        return "Text Field";
    }

    // Generic description
    std::string name = metadata.value("name", "");
    if (!name.empty() && name != type) {
        return type + " (" + name + ")";
    }
    return type;
}

} // namespace sap
} // namespace fairyfly
