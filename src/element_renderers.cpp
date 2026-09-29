#include "include/element_renderers.h"
#include "include/element_renderer_registry.h"
#include "include/semantic_classifier.h"
#include "include/sensitive_data.h"
#include "include/formatters/table_formatter.h"
#include <spdlog/spdlog.h>

namespace fairyfly {
namespace sap {
namespace renderers {

// Helper functions implementation
namespace helpers {
    json render_children_json(const json& element_metadata) {
        if (!element_metadata.contains("children") || !element_metadata["children"].is_array()) {
            return json::array();
        }

        const auto& children = element_metadata["children"];

        // Check if children array is empty
        if (children.empty()) {
            return json::array();
        }

        // Check if children are objects (old format) or strings (new optimized format)
        if (!children[0].is_object()) {
            // Children are ID strings (new format) - skip rendering
            // Full data is available in top-level elements array
            return json::array();
        }

        json filtered_children = json::array();
        auto& registry = ElementRendererRegistry::instance();

        for (const auto& child : children) {
            // Check if child should be displayed
            if (SemanticClassifier::should_display(child)) {
                json rendered = registry.render_to_json(child);
                if (!rendered.empty()) {
                    filtered_children.push_back(rendered);
                }
            }
        }

        return filtered_children;
    }

    std::string render_children_markdown(const json& element_metadata, int indent_level) {
        if (!element_metadata.contains("children") || !element_metadata["children"].is_array()) {
            return "";
        }

        const auto& children = element_metadata["children"];

        // Check if children array is empty
        if (children.empty()) {
            return "";
        }

        // Check if children are objects (old format) or strings (new optimized format)
        if (!children[0].is_object()) {
            // Children are ID strings (new format) - skip rendering
            // Full data is available in top-level elements array
            return "";
        }

        std::ostringstream oss;
        auto& registry = ElementRendererRegistry::instance();

        for (const auto& child : children) {
            try {
                // Check if child should be displayed
                if (SemanticClassifier::should_display(child)) {
                    std::string rendered = registry.render_to_markdown(child, indent_level + 1);
                    if (!rendered.empty()) {
                        oss << rendered;
                    }
                }
            } catch (const std::exception& e) {
                std::string child_id = child.value("id", "<no-id>");
                spdlog::warn("Failed to render child element {}: {}", child_id, e.what());
                // Continue processing other children instead of failing completely
            }
        }

        return oss.str();
    }

    std::string make_indent(int level) {
        return std::string(level * 2, ' ');
    }
}

// SplitterRenderer implementation
json SplitterRenderer::to_json(const json& element_metadata) {
    json result = element_metadata;

    // Recursively process panels (children)
    result["children"] = helpers::render_children_json(element_metadata);

    // Add semantic description
    result["semantic_type"] = "Splitter";
    int panel_count = static_cast<int>(result["children"].size());
    result["semantic_description"] = "Splitter (" + std::to_string(panel_count) + " panels)";

    return result;
}

std::string SplitterRenderer::to_markdown(const json& element_metadata, int indent_level) {
    std::ostringstream oss;
    std::string indent = helpers::make_indent(indent_level);

    // Get description
    std::string description = SemanticClassifier::get_semantic_description("GuiSplitterShell", element_metadata);

    oss << indent << "### " << description << "\n\n";

    // Recursively render panels
    if (element_metadata.contains("children") && element_metadata["children"].is_array()) {
        int panel_idx = 1;
        for (const auto& child : element_metadata["children"]) {
            if (!child.is_object()) continue;
            if (SemanticClassifier::should_display(child)) {
                std::string panel_content = helpers::render_children_markdown(child, indent_level);
                // Only output if panel has content
                if (!panel_content.empty()) {
                    oss << indent << "**Panel " << panel_idx++ << ":**\n";
                    oss << panel_content;
                    oss << "\n";
                }
            }
        }
    }

    return oss.str();
}

// ContainerRenderer implementation
json ContainerRenderer::to_json(const json& element_metadata) {
    // Only render if container has semantic children
    if (!SemanticClassifier::has_semantic_children(element_metadata)) {
        return json::object();  // Empty container - don't render
    }

    json result = element_metadata;
    result["children"] = helpers::render_children_json(element_metadata);
    return result;
}

std::string ContainerRenderer::to_markdown(const json& element_metadata, int indent_level) {
    // Only render if container has semantic children
    if (!SemanticClassifier::has_semantic_children(element_metadata)) {
        return "";
    }

    // Containers are transparent - just render children
    return helpers::render_children_markdown(element_metadata, indent_level);
}

// TreeRenderer implementation
json TreeRenderer::to_json(const json& element_metadata) {
    json result = element_metadata;

    // Add semantic info
    result["semantic_type"] = "Tree";

    // Count nodes if tree_data available
    if (element_metadata.contains("tree_data")) {
        const auto& tree_data = element_metadata["tree_data"];
        if (tree_data.contains("nodes") && tree_data["nodes"].is_array()) {
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
            result["node_count"] = node_count;
            result["semantic_description"] = "Tree (" + std::to_string(node_count) + " nodes)";
        }
    }

    return result;
}

std::string TreeRenderer::to_markdown(const json& element_metadata, int indent_level) {
    std::ostringstream oss;
    std::string indent = helpers::make_indent(indent_level);

    // Get description with node count
    std::string description = SemanticClassifier::get_semantic_description("GuiTree", element_metadata);

    oss << indent << "### " << description << "\n\n";

    // Show tree element ID prominently
    std::string tree_id = element_metadata.value("id", "");
    if (!tree_id.empty()) {
        oss << indent << "**Tree ID:** `" << tree_id << "`\n\n";
    }

    // Support both tree_nodes (new format) and tree_data.nodes (old format)
    bool has_tree_nodes = element_metadata.contains("tree_nodes") && element_metadata["tree_nodes"].is_array();
    bool has_tree_data = element_metadata.contains("tree_data") &&
                         element_metadata["tree_data"].contains("nodes") &&
                         element_metadata["tree_data"]["nodes"].is_array();

    if (has_tree_nodes || has_tree_data) {
        // Get nodes from either location
        const auto& nodes = has_tree_nodes ? element_metadata["tree_nodes"] : element_metadata["tree_data"]["nodes"];

        // Get columns - for new format, get from first node; for old format, get from tree_data
        json columns = json::array();
        if (has_tree_nodes && !nodes.empty() && nodes[0].is_object() && nodes[0].contains("available_columns")) {
            columns = nodes[0]["available_columns"];
        } else if (has_tree_data && element_metadata["tree_data"].contains("columns")) {
            columns = element_metadata["tree_data"]["columns"];
        }

        // Check if nodes are flat with paths (new format from screen_reader)
        bool is_flat_with_paths = false;
        bool has_column_data = false;
        if (!nodes.empty() && nodes[0].is_object()) {
            is_flat_with_paths = nodes[0].contains("path") && nodes[0].contains("text") && nodes[0].contains("key");
            has_column_data = nodes[0].contains("column_values") && !nodes[0]["column_values"].empty();
        }

        // Render flat tree nodes with paths (new format)
        if (is_flat_with_paths) {
            // If we have column data, render as table; otherwise render as simple tree
            if (has_column_data && !columns.empty()) {
                render_flat_tree_table_with_columns(oss, nodes, columns, indent);
            } else {
                render_flat_tree_with_paths(oss, nodes, indent);
            }
        } else {
            // Check if nodes have column_values (indicates table-like data)
            bool has_column_values = false;
            for (const auto& node : nodes) {
                if (node.contains("column_values") && !node["column_values"].empty()) {
                    has_column_values = true;
                    break;
                }
            }

            // Use table format if we have column values, otherwise hierarchical
            if (has_column_values && !columns.empty()) {
                render_table_tree(oss, nodes, columns, indent);
            } else {
                render_hierarchical_tree(oss, nodes, indent);
            }
        }

        // Add usage hint if tree ID is available
        if (!tree_id.empty()) {
            oss << indent << "_Select node by double-clicking tree or use row index with automation tools_\n\n";
        }
    }

    return oss.str();
}

// TreeRenderer private helper methods
int TreeRenderer::calculate_path_depth(const std::string& path) {
    int depth = 0;
    for (char c : path) {
        if (c == '\\') {
            depth++;
        }
    }
    return depth;
}

void TreeRenderer::render_flat_tree_with_paths(std::ostringstream& oss, const json& nodes, const std::string& indent) {
    oss << indent << "```\n";
    for (const auto& node : nodes) {
        if (!node.is_object()) continue;

        const std::string path = node.value("path", "");
        const std::string text = node.value("text", "");

        const int depth = calculate_path_depth(path);
        const std::string tree_indent(depth * TREE_INDENT_SPACES, ' ');
        const std::string branch = depth > 0 ? BRANCH_PREFIX : "";

        oss << indent << tree_indent << branch << text << "\n";
    }
    oss << indent << "```\n\n";
    oss << indent << "_" << nodes.size() << " nodes extracted_\n\n";
}

void TreeRenderer::render_flat_tree_table_with_columns(std::ostringstream& oss, const json& nodes, const json& columns, const std::string& indent) {
    // Build column headers (skip HierarchyHeader, use actual column names)
    std::vector<std::string> display_columns;
    for (const auto& col : columns) {
        std::string col_name = col.is_string() ? col.get<std::string>() : "";
        if (col_name != "HierarchyHeader" && !col_name.empty()) {
            // Clean up column name for display
            std::string display_name = col_name;
            // Remove leading "C " prefix if present
            if (display_name.size() > 2 && display_name[0] == 'C' && display_name[1] == ' ') {
                display_name = display_name.substr(2);
            }
            // Trim whitespace
            size_t start = display_name.find_first_not_of(" \t");
            size_t end = display_name.find_last_not_of(" \t");
            if (start != std::string::npos && end != std::string::npos) {
                display_name = display_name.substr(start, end - start + 1);
            }
            display_columns.push_back(display_name);
        }
    }

    // Table header
    oss << indent << "| RFC Connection |";
    for (const auto& col_name : display_columns) {
        oss << " " << col_name << " |";
    }
    oss << "\n";

    // Table separator
    oss << indent << "|:---------------|";
    for (size_t i = 0; i < display_columns.size(); ++i) {
        oss << ":--------|";
    }
    oss << "\n";

    // Render each node as table row
    for (const auto& node : nodes) {
        if (!node.is_object()) continue;

        const std::string path = node.value("path", "");
        const std::string text = node.value("text", "");
        const auto column_values = node.value("column_values", json::array());

        // Calculate indentation for tree structure in first column
        const int depth = calculate_path_depth(path);
        const std::string tree_indent(depth * TREE_INDENT_SPACES, ' ');
        const std::string branch = depth > 0 ? BRANCH_PREFIX : "";

        // First column: tree hierarchy with text
        oss << indent << "| " << tree_indent << branch << text << " |";

        // Remaining columns: data values (skip HierarchyHeader column)
        size_t col_idx = 0;
        for (size_t i = 0; i < columns.size() && col_idx < display_columns.size(); ++i) {
            std::string col_name = columns[i].is_string() ? columns[i].get<std::string>() : "";
            if (col_name == "HierarchyHeader" || col_name.empty()) {
                continue;  // Skip hierarchy column
            }

            std::string cell_value = "";
            if (i < column_values.size() && column_values[i].is_string()) {
                cell_value = column_values[i].get<std::string>();
            }

            oss << " " << cell_value << " |";
            col_idx++;
        }

        oss << "\n";
    }

    oss << "\n";
    oss << indent << "_" << nodes.size() << " nodes with " << display_columns.size() << " columns_\n\n";
}

void TreeRenderer::render_table_tree(std::ostringstream& oss, const json& nodes, const json& columns, const std::string& indent) {
    try {
        // Table header with row number
        oss << indent << "| # | Text |";
        for (const auto& col : columns) {
            const std::string col_name = col.is_string() ? col.get<std::string>() : "Column";
            const std::string display_name = (col_name == "Name") ? "Technical Name" : col_name;
            oss << " " << display_name << " |";
        }
        oss << "\n" << indent << "|:--|:---------|";
        for (size_t i = 0; i < columns.size(); ++i) {
            oss << "---------------|";
        }
        oss << "\n";

        // Render each node as table row
        for (size_t i = 0; i < nodes.size(); ++i) {
            const auto& node = nodes[i];
            if (!node.is_object()) {
                spdlog::warn("Skipping non-object tree node at index {}", i);
                continue;
            }

            const std::string text = node.value("text", "<empty>");
            const auto column_values = node.value("column_values", json::array());

            oss << indent << "| " << i << " | **" << text << "** |";

            // Handle column/value count mismatch by padding with empty cells
            const size_t col_count = columns.size();
            for (size_t j = 0; j < col_count; ++j) {
                std::string cell_value = "";
                if (j < column_values.size() && column_values[j].is_string()) {
                    cell_value = column_values[j].get<std::string>();
                }
                oss << " " << cell_value << " |";
            }
            oss << "\n";
        }
        oss << "\n";
    } catch (const std::exception& e) {
        spdlog::error("Failed to render tree table: {}", e.what());
        // Fall back to simple tree format
        oss << indent << "_Error rendering tree table, showing simple format_\n\n";
        oss << indent << "```\n";
        for (size_t i = 0; i < nodes.size(); ++i) {
            if (nodes[i].is_object()) {
                const std::string text = nodes[i].value("text", "<empty>");
                oss << indent << (i == nodes.size() - 1 ? LAST_BRANCH_PREFIX : BRANCH_PREFIX) << text << "\n";
            }
        }
        oss << indent << "```\n\n";
    }
}

void TreeRenderer::render_hierarchical_tree(std::ostringstream& oss, const json& nodes, const std::string& indent) {
    oss << indent << "```\n";

    std::function<void(const json&, const std::string&, bool)> render_node;
    render_node = [&](const json& node, const std::string& prefix, bool is_last) {
        if (!node.is_object()) {
            spdlog::warn("Skipping non-object tree node");
            return;
        }

        std::string text = node.value("text", "");
        if (text.empty()) {
            text = "<empty node>";
        }

        oss << indent << prefix << (is_last ? LAST_BRANCH_PREFIX : BRANCH_PREFIX);
        oss << "**" << text << "**";

        const std::string tooltip = node.value("tooltip", "");
        if (!tooltip.empty() && tooltip != text) {
            oss << " (" << tooltip << ")";
        }
        oss << "\n";

        // Render children
        if (node.contains("children") && node["children"].is_array()) {
            try {
                const auto& children = node["children"];
                const size_t child_count = children.size();
                for (size_t i = 0; i < child_count; ++i) {
                    const std::string child_prefix = prefix + (is_last ? "    " : "|   ");
                    const bool child_is_last = (i == child_count - 1);
                    render_node(children[i], child_prefix, child_is_last);
                }
            } catch (const std::exception& e) {
                spdlog::warn("Failed to render tree node children: {}", e.what());
            }
        }
    };

    const size_t node_count = nodes.size();
    for (size_t i = 0; i < node_count; ++i) {
        render_node(nodes[i], "", i == node_count - 1);
    }

    oss << indent << "```\n\n";
}

// ToolbarRenderer implementation
json ToolbarRenderer::to_json(const json& element_metadata) {
    json result = element_metadata;
    result["semantic_type"] = "Toolbar";
    result["children"] = helpers::render_children_json(element_metadata);
    return result;
}

std::string ToolbarRenderer::to_markdown(const json& element_metadata, int indent_level) {
    (void)element_metadata; // Toolbar buttons rendered separately by screen_markdown_formatter
    std::ostringstream oss;
    std::string indent = helpers::make_indent(indent_level);

    oss << indent << "### Toolbar\n\n";

    // Toolbar buttons are now stored in toolbar_buttons metadata array
    // They are rendered separately by screen_markdown_formatter
    // Do NOT render children here to avoid processing synthetic button metadata

    return oss.str();
}

// TableRenderer implementation
json TableRenderer::to_json(const json& element_metadata) {
    json result = element_metadata;
    result["semantic_type"] = "Table";

    // Add table dimensions if available
    const json* data = nullptr;
    if (element_metadata.contains("table_data") && element_metadata["table_data"].is_object())
        data = &element_metadata["table_data"];
    else if (element_metadata.contains("grid_data") && element_metadata["grid_data"].is_object())
        data = &element_metadata["grid_data"];
    if (data) {
        const auto& table_data = *data;
        const auto columns = table_data.value("columns", json::array());
        const auto rows_data = table_data.value("rows", json::array());
        result["row_count"] = table_data.value("total_row_count", static_cast<int>(rows_data.size()));
        result["column_count"] = static_cast<int>(columns.size());

        int rows = result["row_count"];
        int cols = result["column_count"];
        result["semantic_description"] = "Table (" + std::to_string(rows) + " rows, " +
                                        std::to_string(cols) + " cols)";
    }

    return result;
}

std::string TableRenderer::to_markdown(const json& element_metadata, int indent_level) {
    std::ostringstream oss;
    formatters::TableFormatter formatter;
    formatter.format_to_markdown(element_metadata, oss);
    if (indent_level <= 0) return oss.str();

    const std::string indent = helpers::make_indent(indent_level);
    std::ostringstream indented;
    std::istringstream lines(oss.str());
    std::string line;
    while (std::getline(lines, line)) {
        if (!line.empty()) indented << indent;
        indented << line << '\n';
    }
    return indented.str();
}

// ButtonRenderer implementation
json ButtonRenderer::to_json(const json& element_metadata) {
    json result = element_metadata;
    result["semantic_type"] = "Button";

    std::string text = element_metadata.value("text", "");
    if (!text.empty()) {
        result["semantic_description"] = "Button: " + text;
    }

    return result;
}

std::string ButtonRenderer::to_markdown(const json& element_metadata, int indent_level) {
    std::ostringstream oss;
    std::string indent = helpers::make_indent(indent_level);

    std::string text = element_metadata.value("text", "");
    std::string name = element_metadata.value("name", "");

    oss << indent << "- 🔘 Button";
    if (!text.empty()) {
        oss << ": **" << text << "**";
    }
    if (!name.empty()) {
        oss << " `" << name << "`";
    }
    oss << "\n";

    return oss.str();
}

// TextFieldRenderer implementation
json TextFieldRenderer::to_json(const json& element_metadata) {
    json result = element_metadata;
    result["semantic_type"] = "TextField";
    return result;
}

std::string TextFieldRenderer::to_markdown(const json& element_metadata, int indent_level) {
    std::ostringstream oss;
    std::string indent = helpers::make_indent(indent_level);

    std::string name = element_metadata.value("name", "");
    std::string text = element_metadata.value("text", "");

    oss << indent << "- Text Field";
    if (!name.empty()) {
        oss << " `" << name << "`";
    }
    if (!text.empty()) {
        oss << ": \"" << text << "\"";
    }
    oss << "\n";

    return oss.str();
}

// TextEditRenderer implementation
json TextEditRenderer::to_json(const json& element_metadata) {
    json result = element_metadata;

    std::string text = element_metadata.value("text", "");
    std::string id = element_metadata.value("id", "");

    result["semantic_type"] = "TextEdit";

    // Count lines for semantic description
    int line_count = 1;
    for (char c : text) {
        if (c == '\n') line_count++;
    }

    if (line_count > 1) {
        result["semantic_description"] = "Text Editor (" + std::to_string(line_count) + " lines)";
    } else {
        result["semantic_description"] = "Text Editor";
    }

    return result;
}

std::string TextEditRenderer::to_markdown(const json& element_metadata, int indent_level) {
    std::ostringstream oss;
    std::string indent = helpers::make_indent(indent_level);

    std::string text = element_metadata.value("text", "");
    std::string id = element_metadata.value("id", "");
    std::string name = element_metadata.value("name", "");

    // Get semantic description with line count
    std::string description = SemanticClassifier::get_semantic_description("GuiTextedit", element_metadata);

    oss << indent << "## " << description << "\n\n";

    // Show content in code block
    if (!text.empty()) {
        oss << indent << "```\n";
        oss << text << "\n";
        oss << indent << "```\n\n";
    } else {
        oss << indent << "_Empty_\n\n";
    }

    // Show technical ID and name if present
    if (!id.empty()) {
        oss << indent << "**Technical ID:** `" << id << "`\n";
    }
    if (!name.empty()) {
        oss << indent << "**Name:** `" << name << "`\n";
    }
    if (!id.empty() || !name.empty()) {
        oss << "\n";
    }

    return oss.str();
}

// Register all renderers with the registry
void register_all_renderers() {
    auto& registry = ElementRendererRegistry::instance();

    // Register JSON renderers
    registry.register_json_renderer("GuiSplitterShell", SplitterRenderer::to_json);
    registry.register_json_renderer("GuiContainerShell", ContainerRenderer::to_json);
    registry.register_json_renderer("GuiCustomControl", ContainerRenderer::to_json);
    registry.register_json_renderer("GuiSimpleContainer", ContainerRenderer::to_json);
    registry.register_json_renderer("GuiScrollContainer", ContainerRenderer::to_json);
    registry.register_json_renderer("GuiDockShell", ContainerRenderer::to_json);
    registry.register_json_renderer("GuiTree", TreeRenderer::to_json);
    registry.register_json_renderer("GuiGridView", TableRenderer::to_json);
    registry.register_json_renderer("GuiTableControl", TableRenderer::to_json);
    registry.register_json_renderer("GuiButton", ButtonRenderer::to_json);
    registry.register_json_renderer("GuiTextField", TextFieldRenderer::to_json);
    registry.register_json_renderer("GuiCTextField", TextFieldRenderer::to_json);
    registry.register_json_renderer("GuiTextedit", TextEditRenderer::to_json);

    // Register Markdown renderers
    registry.register_markdown_renderer("GuiSplitterShell", SplitterRenderer::to_markdown);
    registry.register_markdown_renderer("GuiContainerShell", ContainerRenderer::to_markdown);
    registry.register_markdown_renderer("GuiCustomControl", ContainerRenderer::to_markdown);
    registry.register_markdown_renderer("GuiSimpleContainer", ContainerRenderer::to_markdown);
    registry.register_markdown_renderer("GuiScrollContainer", ContainerRenderer::to_markdown);
    registry.register_markdown_renderer("GuiDockShell", ContainerRenderer::to_markdown);
    registry.register_markdown_renderer("GuiTree", TreeRenderer::to_markdown);
    registry.register_markdown_renderer("GuiGridView", TableRenderer::to_markdown);
    registry.register_markdown_renderer("GuiTableControl", TableRenderer::to_markdown);
    registry.register_markdown_renderer("GuiButton", ButtonRenderer::to_markdown);
    registry.register_markdown_renderer("GuiTextField", TextFieldRenderer::to_markdown);
    registry.register_markdown_renderer("GuiCTextField", TextFieldRenderer::to_markdown);
    registry.register_markdown_renderer("GuiTextedit", TextEditRenderer::to_markdown);

    // Handle GuiShell specially - it can be tree, toolbar, grid, or textedit depending on SubType
    registry.register_json_renderer("GuiShell", [](const json& metadata) -> json {
        std::string subtype = metadata.value("subtype", "");
        if (subtype == "Tree" || metadata.contains("tree_data")) {
            return TreeRenderer::to_json(metadata);
        }
        if (subtype == "GridView" || (subtype != "Toolbar" && metadata.contains("table_data"))) {
            return TableRenderer::to_json(metadata);
        }
        if (subtype == "Toolbar") {
            return ToolbarRenderer::to_json(metadata);
        }
        if (subtype == "TextEdit") {
            json result = metadata;
            result["semantic_type"] = "TextEdit";
            std::string text = metadata.value("text", "");
            result["semantic_description"] = "Text Editor (" + std::to_string(text.length()) + " chars)";
            return result;
        }
        if (subtype == "HTMLViewer") {
            json result = metadata;
            result["semantic_type"] = "HTMLViewer";
            if (metadata.value("content_available", false) &&
                !metadata.value("text", "").empty()) {
                result["text"] = redact_sensitive_response_text(metadata.value("text", ""));
                result["content_available"] = true;
            } else {
                result.erase("text"); // SAP returns the control name, not the page body.
                result["content_available"] = false;
            }
            return result;
        }
        return ContainerRenderer::to_json(metadata);
    });

    registry.register_markdown_renderer("GuiShell", [](const json& metadata, int level) -> std::string {
        std::string subtype = metadata.value("subtype", "");
        if (subtype == "Tree" || metadata.contains("tree_data")) {
            return TreeRenderer::to_markdown(metadata, level);
        }
        if (subtype == "GridView" || (subtype != "Toolbar" && metadata.contains("table_data"))) {
            return TableRenderer::to_markdown(metadata, level);
        }
        if (subtype == "Toolbar") {
            return ToolbarRenderer::to_markdown(metadata, level);
        }
        if (subtype == "TextEdit") {
            std::ostringstream oss;
            std::string indent = helpers::make_indent(level);

            std::string text = metadata.value("text", "");
            std::string id = metadata.value("id", "");

            oss << indent << "### Text Editor\n\n";

            // Show content in code block
            if (!text.empty()) {
                oss << indent << "```\n";
                oss << text << "\n";
                oss << indent << "```\n\n";
            } else {
                oss << indent << "_Empty_\n\n";
            }

            // Show technical ID
            if (!id.empty()) {
                oss << indent << "**Technical ID:** `" << id << "`\n\n";
            }

            return oss.str();
        }
        if (subtype == "HTMLViewer") {
            std::ostringstream oss;
            std::string indent = helpers::make_indent(level);
            oss << indent << "### HTML viewer\n\n";
            if (metadata.value("content_available", false) &&
                !metadata.value("text", "").empty()) {
                oss << indent << "```text\n";
                oss << redact_sensitive_response_text(metadata.value("text", "")) << "\n";
                oss << indent << "```\n\n";
                if (metadata.value("content_truncated", false)) {
                    oss << indent << "_Page text truncated._\n\n";
                }
            } else {
                oss << indent << "_Page content unavailable through SAP GUI scripting and UI Automation._\n\n";
            }
            const std::string id = metadata.value("id", "");
            if (!id.empty()) {
                oss << indent << "**Technical ID:** `" << id << "`\n\n";
            }
            return oss.str();
        }
        return ContainerRenderer::to_markdown(metadata, level);
    });
}

} // namespace renderers
} // namespace sap
} // namespace fairyfly
