#include "include/element_renderer_registry.h"
#include "include/semantic_classifier.h"
#include <sstream>

namespace fairyfly {
namespace sap {

ElementRendererRegistry::ElementRendererRegistry() {
    // Constructor initializes default renderers
    // Specialized renderers will be registered later
}

ElementRendererRegistry& ElementRendererRegistry::instance() {
    static ElementRendererRegistry instance;
    return instance;
}

void ElementRendererRegistry::register_json_renderer(const std::string& type, JsonRenderer renderer) {
    json_renderers_[type] = renderer;
}

void ElementRendererRegistry::register_markdown_renderer(const std::string& type, MarkdownRenderer renderer) {
    markdown_renderers_[type] = renderer;
}

json ElementRendererRegistry::render_to_json(const json& element_metadata) const {
    std::string type = element_metadata.value("type", "");

    // Look up renderer for this type
    auto it = json_renderers_.find(type);
    if (it != json_renderers_.end()) {
        return it->second(element_metadata);
    }

    // Fall back to default renderer
    return default_json_renderer(element_metadata);
}

std::string ElementRendererRegistry::render_to_markdown(const json& element_metadata, int indent_level) const {
    std::string type = element_metadata.value("type", "");

    // Look up renderer for this type
    auto it = markdown_renderers_.find(type);
    if (it != markdown_renderers_.end()) {
        return it->second(element_metadata, indent_level);
    }

    // Fall back to default renderer
    return default_markdown_renderer(element_metadata, indent_level);
}

bool ElementRendererRegistry::has_json_renderer(const std::string& type) const {
    return json_renderers_.find(type) != json_renderers_.end();
}

bool ElementRendererRegistry::has_markdown_renderer(const std::string& type) const {
    return markdown_renderers_.find(type) != markdown_renderers_.end();
}

json ElementRendererRegistry::default_json_renderer(const json& element_metadata) const {
    // Default: return metadata as-is, but filter children recursively if semantic classifier says to hide
    json result = element_metadata;

    // Check if this element should be displayed
    if (!SemanticClassifier::should_display(element_metadata)) {
        return json::object();  // Return empty object for layout-only elements
    }

    // Recursively filter children
    if (result.contains("children") && result["children"].is_array()) {
        json filtered_children = json::array();
        for (const auto& child : result["children"]) {
            json filtered_child = render_to_json(child);
            if (!filtered_child.empty()) {
                filtered_children.push_back(filtered_child);
            }
        }
        result["children"] = filtered_children;
    }

    return result;
}

std::string ElementRendererRegistry::default_markdown_renderer(const json& element_metadata, int indent_level) const {
    // Check if this element should be displayed
    if (!SemanticClassifier::should_display(element_metadata)) {
        return "";  // Don't render layout-only elements
    }

    std::ostringstream oss;
    std::string indent(indent_level * 2, ' ');

    // Get semantic description
    std::string type = element_metadata.value("type", "");
    std::string description = SemanticClassifier::get_semantic_description(type, element_metadata);

    // Render this element
    oss << indent << "- " << description << "\n";

    // Recursively render children
    if (element_metadata.contains("children") && element_metadata["children"].is_array()) {
        for (const auto& child : element_metadata["children"]) {
            std::string child_md = render_to_markdown(child, indent_level + 1);
            if (!child_md.empty()) {
                oss << child_md;
            }
        }
    }

    return oss.str();
}

} // namespace sap
} // namespace fairyfly
