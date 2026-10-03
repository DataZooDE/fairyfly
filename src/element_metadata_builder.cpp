#include "include/element_metadata_builder.h"
#include "include/constants.h"
#include "include/element_type_registry.h"
#include <algorithm>

namespace fairyfly {
namespace sap {

using nlohmann::json;

bool metadata_reads_changeable(const std::string& type) {
    // Only input controls can be changeable
    return type == "GuiTextField" || type == "GuiCTextField" ||
           type == "GuiPasswordField" || type == "GuiOkCodeField" ||
           type == "GuiComboBox" || type == "GuiComboBoxControl" ||
           type == "GuiCheckBox" || type == "GuiRadioButton";
}

bool metadata_reads_selected(const std::string& type) {
    return type == "GuiCheckBox" || type == "GuiRadioButton";
}

bool metadata_reads_label(const std::string& type) {
    // Accessibility labels - only input controls have an associated label
    return !is_non_visual_container(type) &&
           (type == "GuiTextField" || type == "GuiCTextField" ||
            type == "GuiPasswordField" || type == "GuiComboBox" ||
            type == "GuiComboBoxControl" || type == "GuiCheckBox" ||
            type == "GuiRadioButton");
}

bool metadata_reads_tooltip(const std::string& type) {
    return !is_non_visual_container(type) &&
           (type == "GuiButton" || type == "GuiTextField" || type == "GuiCTextField" ||
            type == "GuiPasswordField" || type == "GuiTab" || type == "GuiComboBox" ||
            type == "GuiStatusPane");
}

std::string classify_container_type(const std::string& type, const std::function<int()>& child_count) {
    // Leaf elements are never containers - fast path avoids a COM Children query
    if (type == "GuiLabel" || type == "GuiButton" || type == "GuiTextField" ||
        type == "GuiCTextField" || type == "GuiPasswordField" || type == "GuiOkCodeField" ||
        type == "GuiCheckBox" || type == "GuiRadioButton" || type == "GuiComboBox" ||
        type == "GuiComboBoxControl" || type == "GuiStatusPane") {
        return "";
    }

    // Classify containers by SAP GUI type
    if (type == "GuiToolbar" || type == "GuiMenubar") return "toolbar";
    if (type == "GuiTabStrip") return "tabs";
    if (type == "GuiTableControl") return "table";
    if (type == "GuiGridView") return "grid";
    if (type == "GuiUserArea" || type == "GuiSimpleContainer") return "form";
    if (type == "GuiStatusPane") return "statusbar";
    if (type == "GuiTitlebar") return "titlebar";
    if (type == "GuiBox") return "group";

    // Check if element has children (generic container)
    if (child_count() > 0) return "container";

    return "";
}

bool metadata_lists_children(const std::string& type, const std::string& container_type) {
    // GuiUserArea children are collected by the traversal (positioned labels become cells).
    if (type == "GuiUserArea") return false;
    return !container_type.empty() || type == "GuiContainerShell" ||
           type == "GuiCustomControl" || type == "GuiSplitterContainer" ||
           type == "GuiContainerCtrl" || type == "GuiSplitterShell" ||
           type == "GuiDockShell" || type == "GuiTabStrip" ||
           type == "GuiToolbar" || type == "GuiMenubar";
}

void add_child_ids(json& metadata, const std::vector<std::string>& child_ids) {
    if (child_ids.empty()) return;
    const auto count = (std::min)(child_ids.size(),
                                  static_cast<size_t>(constants::MAX_CHILDREN_TO_PROCESS));
    json children = json::array();
    for (size_t i = 0; i < count; ++i) children.push_back(child_ids[i]);
    metadata["children"] = children;
    metadata["child_count"] = children.size();
}

json build_plain_metadata(const ElementFacts& facts) {
    const auto& type = facts.type;
    json metadata;
    metadata["id"] = facts.id;
    metadata["type"] = type;
    metadata["name"] = facts.name;
    metadata["text"] = facts.text;
    metadata["enabled"] = facts.enabled;
    metadata["visible"] = facts.visible;
    metadata["changeable"] = facts.changeable;
    if (metadata_reads_selected(type) && facts.selected) metadata["selected"] = *facts.selected;

    if (metadata_reads_label(type) && !facts.label.empty()) metadata["label"] = facts.label;
    if (metadata_reads_tooltip(type) && !facts.tooltip.empty()) metadata["tooltip"] = facts.tooltip;

    if (!facts.container_type.empty()) metadata["container_type"] = facts.container_type;

    // Parse grid coordinates for grid-positioned labels (pattern: lbl[row,col])
    if (type == "GuiLabel" && facts.id.find("/lbl[") != std::string::npos) {
        size_t bracket_pos = facts.id.find("/lbl[");
        size_t comma_pos = facts.id.find(",", bracket_pos);
        size_t close_bracket = facts.id.find("]", comma_pos);

        if (comma_pos != std::string::npos && close_bracket != std::string::npos) {
            try {
                // SAP GUI coordinate notation is /lbl[col,row] (column character offset, line row number)
                std::string col_str = facts.id.substr(bracket_pos + 5, comma_pos - bracket_pos - 5);
                std::string row_str = facts.id.substr(comma_pos + 1, close_bracket - comma_pos - 1);

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
        metadata["subtype"] = facts.subtype.empty() ? "N/A" : facts.subtype;
    }

    // Special handling for GuiBox (grouping container)
    if (type == "GuiBox") {
        metadata["is_group"] = true;
    }

    // Derive capabilities
    metadata["capabilities"] = derive_capabilities(type, facts.enabled, facts.changeable);

    // GuiCTextField always has F4 help (the "C" stands for "Combo"/search)
    if (type == "GuiCTextField") {
        metadata["has_f4_help"] = true;
        if (metadata["capabilities"].is_array()) {
            metadata["capabilities"].push_back("has_f4_help");
        }
    }

    add_child_ids(metadata, facts.child_ids);
    return metadata;
}

} // namespace sap
} // namespace fairyfly
