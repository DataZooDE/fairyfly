#include "include/element_renderers.h"
#include "include/element_renderer_registry.h"
#include "include/semantic_classifier.h"

namespace fairyfly {
namespace sap {
namespace renderers {

// Helper functions implementation
namespace helpers {
    json render_children_json(const json& element_metadata) {
        if (!element_metadata.contains("children") || !element_metadata["children"].is_array()) {
            return json::array();
        }

        json filtered_children = json::array();
        auto& registry = ElementRendererRegistry::instance();

        for (const auto& child : element_metadata["children"]) {
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

        std::ostringstream oss;
        auto& registry = ElementRendererRegistry::instance();

        for (const auto& child : element_metadata["children"]) {
            // Check if child should be displayed
            if (SemanticClassifier::should_display(child)) {
                std::string rendered = registry.render_to_markdown(child, indent_level + 1);
                if (!rendered.empty()) {
                    oss << rendered;
                }
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

    // Render tree structure if available
    if (element_metadata.contains("tree_data")) {
        const auto& tree_data = element_metadata["tree_data"];
        if (tree_data.contains("nodes") && tree_data["nodes"].is_array()) {
            oss << indent << "```\n";

            std::function<void(const json&, const std::string&, bool)> render_node;
            render_node = [&](const json& node, const std::string& prefix, bool is_last) {
                std::string text = node.value("text", "");

                oss << indent << prefix << (is_last ? "+-- " : "|-- ");
                oss << "**" << text << "**";

                std::string tooltip = node.value("tooltip", "");
                if (!tooltip.empty() && tooltip != text) {
                    oss << " (" << tooltip << ")";
                }
                oss << "\n";

                // Render children
                if (node.contains("children") && node["children"].is_array()) {
                    const auto& children = node["children"];
                    size_t child_count = children.size();
                    for (size_t i = 0; i < child_count; ++i) {
                        std::string child_prefix = prefix + (is_last ? "    " : "|   ");
                        bool child_is_last = (i == child_count - 1);
                        render_node(children[i], child_prefix, child_is_last);
                    }
                }
            };

            const auto& nodes = tree_data["nodes"];
            size_t node_count = nodes.size();
            for (size_t i = 0; i < node_count; ++i) {
                render_node(nodes[i], "", i == node_count - 1);
            }

            oss << indent << "```\n\n";
        }
    }

    return oss.str();
}

// ToolbarRenderer implementation
json ToolbarRenderer::to_json(const json& element_metadata) {
    json result = element_metadata;
    result["semantic_type"] = "Toolbar";
    result["children"] = helpers::render_children_json(element_metadata);
    return result;
}

std::string ToolbarRenderer::to_markdown(const json& element_metadata, int indent_level) {
    std::ostringstream oss;
    std::string indent = helpers::make_indent(indent_level);

    oss << indent << "### Toolbar\n\n";

    // Render buttons/items
    oss << helpers::render_children_markdown(element_metadata, indent_level);

    return oss.str();
}

// TableRenderer implementation
json TableRenderer::to_json(const json& element_metadata) {
    json result = element_metadata;
    result["semantic_type"] = "Table";

    // Add table dimensions if available
    if (element_metadata.contains("table_data")) {
        const auto& table_data = element_metadata["table_data"];
        result["row_count"] = table_data.value("row_count", 0);
        result["column_count"] = table_data.value("column_count", 0);

        int rows = result["row_count"];
        int cols = result["column_count"];
        result["semantic_description"] = "Table (" + std::to_string(rows) + " rows, " +
                                        std::to_string(cols) + " cols)";
    }

    return result;
}

std::string TableRenderer::to_markdown(const json& element_metadata, int indent_level) {
    std::ostringstream oss;
    std::string indent = helpers::make_indent(indent_level);

    std::string description = SemanticClassifier::get_semantic_description("GuiTableControl", element_metadata);

    oss << indent << "### " << description << "\n\n";

    // TODO: Render table data if available

    return oss.str();
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
        return ContainerRenderer::to_markdown(metadata, level);
    });
}

} // namespace renderers
} // namespace sap
} // namespace fairyfly
