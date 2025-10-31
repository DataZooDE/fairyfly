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
