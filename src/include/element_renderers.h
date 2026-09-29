#pragma once

#include "element_renderer_registry.h"
#include "semantic_classifier.h"
#include <nlohmann/json.hpp>
#include <string>
#include <sstream>

namespace fairyfly {
namespace sap {
namespace renderers {

using json = nlohmann::json;

/// Initialize all specialized renderers
void register_all_renderers();

/// Splitter renderer - recursively unfolds panels
class SplitterRenderer {
public:
    static json to_json(const json& element_metadata);
    static std::string to_markdown(const json& element_metadata, int indent_level);
};

/// Container renderer - filters out empty layout containers
class ContainerRenderer {
public:
    static json to_json(const json& element_metadata);
    static std::string to_markdown(const json& element_metadata, int indent_level);
};

/// Tree renderer - enhanced display with node count
class TreeRenderer {
public:
    static json to_json(const json& element_metadata);
    static std::string to_markdown(const json& element_metadata, int indent_level);

private:
    // Named constants for tree rendering
    static constexpr int TREE_INDENT_SPACES = 2;
    static constexpr const char* BRANCH_PREFIX = "|-- ";
    static constexpr const char* LAST_BRANCH_PREFIX = "+-- ";

    // Helper methods for tree rendering
    static int calculate_path_depth(const std::string& path);
    static void render_flat_tree_with_paths(std::ostringstream& oss, const json& nodes, const std::string& indent);
    static void render_flat_tree_table_with_columns(std::ostringstream& oss, const json& nodes, const json& columns, const std::string& indent);
    static void render_table_tree(std::ostringstream& oss, const json& nodes, const json& columns, const std::string& indent);
    static void render_hierarchical_tree(std::ostringstream& oss, const json& nodes, const std::string& indent);
};

/// Toolbar renderer - displays buttons and actions
class ToolbarRenderer {
public:
    static json to_json(const json& element_metadata);
    static std::string to_markdown(const json& element_metadata, int indent_level);
};

/// Table renderer - displays with row/column info
class TableRenderer {
public:
    static json to_json(const json& element_metadata);
    static std::string to_markdown(const json& element_metadata, int indent_level);
};

/// Button renderer - shows text and state
class ButtonRenderer {
public:
    static json to_json(const json& element_metadata);
    static std::string to_markdown(const json& element_metadata, int indent_level);
};

/// TextField renderer - shows field info
class TextFieldRenderer {
public:
    static json to_json(const json& element_metadata);
    static std::string to_markdown(const json& element_metadata, int indent_level);
};

/// TextEdit renderer - displays multi-line text content for reports and logs
class TextEditRenderer {
public:
    static json to_json(const json& element_metadata);
    static std::string to_markdown(const json& element_metadata, int indent_level);
};

// Helper functions used by multiple renderers
namespace helpers {
    /// Recursively render children using registry
    json render_children_json(const json& element_metadata);

    /// Recursively render children to markdown using registry
    std::string render_children_markdown(const json& element_metadata, int indent_level);

    /// Create indent string
    std::string make_indent(int level);
}

} // namespace renderers
} // namespace sap
} // namespace fairyfly
