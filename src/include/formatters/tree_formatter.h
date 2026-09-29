#pragma once

#include "element_formatter.h"

namespace fairyfly {
namespace formatters {

/// Formatter for GuiTree elements with hierarchical node display
class TreeFormatter : public ElementFormatter {
public:
    void format_to_markdown(const json& element_json, std::ostringstream& oss) const override;
    bool can_format(const std::string& type) const override;

private:
    /// Format a single tree node with indentation
    void format_tree_node(
        const json& node,
        std::ostringstream& oss,
        const std::string& prefix,
        bool is_last,
        int level
    ) const;

    /// Format a tree node as table row with row number and column values
    void format_tree_node_table(
        const json& node,
        std::ostringstream& oss,
        int row_index,
        int level
    ) const;

    /// Get tree branch character based on position
    std::string get_branch_char(bool is_last) const;
};

} // namespace formatters
} // namespace fairyfly
