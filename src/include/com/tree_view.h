#pragma once
// The tree methods of a GuiTree or a tree shell, as a view over its ComGuiElement (the element stays the owner).
#include <string>
#include <vector>

#include "include/com/wrapper.h"

namespace fairyfly {
namespace sap {

class ComGuiTree {
public:
    explicit ComGuiTree(ComGuiElement& element) : element_(element), dispatch_(element.get_com_object()) {}

    /// Get all node keys from a tree control (GuiShell with SubType="Tree")
    /// Returns empty vector if element is not a tree or operation fails
    std::vector<std::string> get_all_node_keys() const;
    std::vector<std::string> get_tree_column_names() const;
    /// Get node text by key from a tree control
    /// Returns empty string if node not found or operation fails
    std::string get_node_text_by_key(const std::string& key) const;
    /// Get node path by key (e.g., "1\2\3" for hierarchical position)
    /// Returns empty string if node not found or operation fails
    std::string get_node_path_by_key(const std::string& key) const;
    /// Get item text from tree node by column name
    /// Returns empty string if not found or operation fails
    std::string get_item_text(const std::string& node_key, const std::string& column_name) const;
    /// Select a tree node without expanding it
    /// @param node_key Node identifier (from GetAllNodeKeys)
    /// Throws ComException if element is not a tree or node not found
    void select_node(const std::string& node_key);
    /// Expand a collapsed tree node
    /// @param node_key Node identifier (from GetAllNodeKeys)
    /// Throws ComException if element is not a tree or node not found
    void expand_node(const std::string& node_key);
    /// Collapse an expanded tree node
    /// @param node_key Node identifier (from GetAllNodeKeys)
    /// Throws ComException if element is not a tree or node not found
    void collapse_node(const std::string& node_key);
    /// Double-click a tree node (default action: expand/collapse or drill-in)
    /// @param node_key Node identifier (from GetAllNodeKeys)
    /// Throws ComException if element is not a tree or node not found
    void doubleclick_node(const std::string& node_key);
    /// Open a tree node's context menu and select an item by its visible text.
    void select_node_context_item(const std::string& node_key, const std::string& item_text);

private:
    ComGuiElement& element_;
    IDispatch* dispatch_;
};

inline ComGuiTree as_tree(ComGuiElement& element) { return ComGuiTree(element); }

} // namespace sap
} // namespace fairyfly
