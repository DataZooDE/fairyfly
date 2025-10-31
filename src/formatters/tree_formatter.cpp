#include "include/formatters/tree_formatter.h"
#include <spdlog/spdlog.h>

namespace fairyfly {
namespace formatters {

bool TreeFormatter::can_format(const std::string& type) const {
    return type == "GuiTree";
}

void TreeFormatter::format_to_markdown(const json& element_json, std::ostringstream& oss) const {
    std::string name = element_json.value("name", "");
    std::string id = element_json.value("id", "");

    // Header
    oss << "### " << (name.empty() ? "Tree" : escape_markdown(name)) << "\n\n";

    // Check if we have tree_data
    if (!element_json.contains("tree_data")) {
        oss << "_Tree structure not available_\n\n";
        oss << "**Technical ID:** `" << id << "`\n\n";
        return;
    }

    const auto& tree_data = element_json["tree_data"];
    const auto& nodes = tree_data.value("nodes", json::array());
    const auto& columns = tree_data.value("columns", json::array());

    // Display column headers if this is a column tree
    if (!columns.empty() && columns.size() > 1) {
        oss << "| Structure |";
        for (const auto& col : columns) {
            std::string col_name = col.get<std::string>();

            // Map SAP internal column names to friendly names
            std::string display_name;
            if (col_name.find("C") == 0) {
                // Column format is "C          1", "C          3", etc.
                std::string trimmed = col_name;
                // Remove extra spaces
                trimmed.erase(std::unique(trimmed.begin(), trimmed.end(),
                    [](char a, char b) { return a == ' ' && b == ' '; }), trimmed.end());

                // Map common column positions to names
                if (trimmed == "C 1") display_name = "Type";
                else if (trimmed == "C 3") display_name = "PL";
                else if (trimmed == "C 4") display_name = "Comment";
                else display_name = trimmed;
            } else {
                display_name = col_name;
            }

            oss << " " << escape_markdown(display_name) << " |";
        }
        oss << "\n|-----------|";
        for (size_t i = 0; i < columns.size(); ++i) {
            oss << "----------|";
        }
        oss << "\n";
    } else {
        // Simple tree with no columns
        oss << "```\n";
    }

    // Format each top-level node
    for (size_t i = 0; i < nodes.size(); ++i) {
        bool is_last = (i == nodes.size() - 1);
        format_tree_node(nodes[i], oss, "", is_last, 0);
    }

    if (columns.empty() || columns.size() <= 1) {
        oss << "```\n";
    }

    oss << "\n**Technical ID:** `" << id << "`\n\n";

    // Add note about total nodes
    oss << "_Showing " << nodes.size() << " top-level nodes_\n\n";
}

void TreeFormatter::format_tree_node(
    const json& node,
    std::ostringstream& oss,
    const std::string& prefix,
    bool is_last,
    int level
) const {
    std::string text = node.value("text", "");
    bool expanded = node.value("expanded", false);
    auto column_values = node.value("column_values", json::array());
    auto children = node.value("children", json::array());

    // Build the tree structure prefix
    std::string branch = get_branch_char(is_last);

    // Check if we're in table format (has column values)
    bool is_table_format = !column_values.empty();

    if (is_table_format) {
        // Table row format
        oss << "| " << prefix << branch << " **"
            << escape_markdown(text) << "** |";

        for (const auto& val : column_values) {
            std::string cell_value = val.is_string() ? val.get<std::string>() : "";
            oss << " " << escape_markdown(cell_value) << " |";
        }
        oss << "\n";
    } else {
        // Simple tree format
        oss << prefix << branch << " " << escape_markdown(text) << "\n";
    }

    // Recursively format children
    if (!children.empty()) {
        std::string child_prefix = prefix + (is_last ? "    " : "|   ");

        for (size_t i = 0; i < children.size(); ++i) {
            bool child_is_last = (i == children.size() - 1);
            format_tree_node(children[i], oss, child_prefix, child_is_last, level + 1);
        }
    }
}

std::string TreeFormatter::get_branch_char(bool is_last) const {
    return is_last ? "+--" : "|--";
}

} // namespace formatters
} // namespace fairyfly
