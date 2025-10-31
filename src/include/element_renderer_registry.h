#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <functional>
#include <map>

namespace fairyfly {
namespace sap {

using json = nlohmann::json;

/// Function type for rendering an element to JSON
using JsonRenderer = std::function<json(const json& element_metadata)>;

/// Function type for rendering an element to Markdown
using MarkdownRenderer = std::function<std::string(const json& element_metadata, int indent_level)>;

/// Registry for type-based element rendering
/// Maps SAP GUI element types to specialized renderers for JSON and Markdown output
class ElementRendererRegistry {
public:
    /// Get singleton instance
    static ElementRendererRegistry& instance();

    /// Register a JSON renderer for a specific element type
    /// @param type SAP GUI type (e.g., "GuiTree", "GuiSplitterShell")
    /// @param renderer Function to convert element metadata to JSON
    void register_json_renderer(const std::string& type, JsonRenderer renderer);

    /// Register a Markdown renderer for a specific element type
    /// @param type SAP GUI type
    /// @param renderer Function to convert element metadata to Markdown
    void register_markdown_renderer(const std::string& type, MarkdownRenderer renderer);

    /// Render element to JSON using registered renderer
    /// @param element_metadata Element metadata including type
    /// @return JSON representation (uses default renderer if type not registered)
    json render_to_json(const json& element_metadata) const;

    /// Render element to Markdown using registered renderer
    /// @param element_metadata Element metadata including type
    /// @param indent_level Indentation level for nested elements
    /// @return Markdown string (uses default renderer if type not registered)
    std::string render_to_markdown(const json& element_metadata, int indent_level = 0) const;

    /// Check if a type has a registered JSON renderer
    bool has_json_renderer(const std::string& type) const;

    /// Check if a type has a registered Markdown renderer
    bool has_markdown_renderer(const std::string& type) const;

private:
    ElementRendererRegistry();  // Private constructor for singleton

    std::map<std::string, JsonRenderer> json_renderers_;
    std::map<std::string, MarkdownRenderer> markdown_renderers_;

    // Default renderers (fallback)
    json default_json_renderer(const json& element_metadata) const;
    std::string default_markdown_renderer(const json& element_metadata, int indent_level) const;
};

} // namespace sap
} // namespace fairyfly
