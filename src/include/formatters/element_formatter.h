#pragma once

#include <nlohmann/json.hpp>
#include <string>
#include <sstream>

namespace fairyfly {
namespace formatters {

using json = nlohmann::json;

/// Base interface for element formatters following Strategy pattern
/// Allows different formatting strategies for different element types
class ElementFormatter {
public:
    virtual ~ElementFormatter() = default;

    /// Format element metadata to markdown
    /// @param element_json The JSON metadata for the element
    /// @param oss Output string stream to write markdown to
    virtual void format_to_markdown(const json& element_json, std::ostringstream& oss) const = 0;

    /// Check if this formatter can handle the given element type
    /// @param type The SAP GUI element type string (e.g., "GuiTree", "GuiGridView")
    /// @return true if this formatter handles this type
    virtual bool can_format(const std::string& type) const = 0;
};

/// Helper function to escape markdown special characters
inline std::string escape_markdown(const std::string& text) {
    std::string result = text;
    // Replace pipe characters that would break tables
    size_t pos = 0;
    while ((pos = result.find("|", pos)) != std::string::npos) {
        result.replace(pos, 1, "\\|");
        pos += 2;
    }
    return result;
}

} // namespace formatters
} // namespace fairyfly
