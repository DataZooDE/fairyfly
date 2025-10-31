#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace fairyfly {
namespace sap {

using json = nlohmann::json;

/// Classifies SAP GUI elements into semantic categories
/// Helps distinguish between meaningful content (trees, tables) and pure layout (containers)
class SemanticClassifier {
public:
    /// Semantic category of an element
    enum class ElementCategory {
        SEMANTIC,      ///< Has semantic meaning: Tree, Table, Toolbar, Button, TextField
        LAYOUT,        ///< Pure layout: ContainerShell without semantic children
        STRUCTURAL     ///< Important structure marker: Splitter, Frame
    };

    /// Classify an element based on its type and metadata
    /// @param type SAP GUI type string (e.g., "GuiTree", "GuiContainerShell")
    /// @param metadata JSON metadata about the element (children, tree_data, etc.)
    /// @return Category classification
    static ElementCategory classify(const std::string& type, const json& metadata);

    /// Check if element should be displayed in output
    /// @param elem_json Element JSON metadata
    /// @return true if element has semantic value worth displaying
    static bool should_display(const json& elem_json);

    /// Check if element is a pure layout container
    /// @param type SAP GUI type string
    /// @return true if element is only for layout (ContainerShell, etc.)
    static bool is_layout_only(const std::string& type);

    /// Check if element has semantic children
    /// @param elem_json Element JSON with children array
    /// @return true if any child has semantic meaning
    static bool has_semantic_children(const json& elem_json);

    /// Get human-readable description of element's semantic role
    /// @param type SAP GUI type string
    /// @param metadata Element metadata
    /// @return Description like "Data Tree", "Action Toolbar", "Empty Panel"
    static std::string get_semantic_description(const std::string& type, const json& metadata);
};

} // namespace sap
} // namespace fairyfly
