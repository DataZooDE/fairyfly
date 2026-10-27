#pragma once
// The tree methods of a GuiTree or a tree shell, as a view over its ComGuiElement (the element stays the owner).
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "include/com/wrapper.h"
#include "include/table_data_extractor.h"   // recover_tree_node_text

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

    /// The node's item text in each column, in order; a failed read is "" (get_item_text). With `skip_hierarchy` a
    /// column without a name or the hierarchy column ("HierarchyHeader", the node text itself) is "" and not asked.
    std::vector<std::string> item_texts(const std::string& key, const std::vector<std::string>& columns,
                                        bool skip_hierarchy) const;

private:
    std::string string_by_key(const wchar_t* method, const std::string& key, const char* operation) const;

    ComGuiElement& element_;
    IDispatch* dispatch_;
};

inline ComGuiTree as_tree(ComGuiElement& element) { return ComGuiTree(element); }

/// The texts of a tree's nodes: a node whose text is empty gets its first non-empty column item
/// (recover_tree_node_text); the tree's column names are read once, when the first such node needs them.
/// Throws as the reads do; the caller marks the node.
class TreeNodeTexts {
public:
    using NodeText = std::function<std::string(const std::string& key)>;
    using ColumnNames = std::function<std::vector<std::string>()>;
    using ItemText = std::function<std::string(const std::string& key, const std::string& column)>;
    TreeNodeTexts(NodeText node_text, ColumnNames column_names, ItemText item_text)
        : node_text_(std::move(node_text)), column_names_(std::move(column_names)), item_text_(std::move(item_text)) {}
    /// Over a tree control (the element must outlive this).
    explicit TreeNodeTexts(ComGuiElement& tree)
        : TreeNodeTexts([&tree](const std::string& key) { return as_tree(tree).get_node_text_by_key(key); },
                        [&tree] { return as_tree(tree).get_tree_column_names(); },
                        [&tree](const std::string& key, const std::string& column) { return as_tree(tree).get_item_text(key, column); }) {}

    std::string text(const std::string& key) {
        const std::string own = node_text_(key);
        if (own.empty() && !names_loaded_) {
            names_ = column_names_();
            names_loaded_ = true;
        }
        return recover_tree_node_text(own, names_, [&](const std::string& column) { return item_text_(key, column); });
    }

private:
    NodeText node_text_;
    ColumnNames column_names_;
    ItemText item_text_;
    std::vector<std::string> names_;
    bool names_loaded_ = false;
};

} // namespace sap
} // namespace fairyfly
