#pragma once

#include <nlohmann/json.hpp>
#include <string>

namespace fairyfly {
namespace cli {

/// Formats SAP GUI screen data as markdown
/// Extracted from cli_handler.cpp to improve code organization
class ScreenMarkdownFormatter {
public:
    /// Format screen data as markdown
    /// @param data JSON object containing screen data with hierarchy and elements
    /// @param compact If true, hide technical IDs and collapse empty fields for more concise output
    /// @return Formatted markdown string
    static std::string format(const nlohmann::json& data, bool compact = false);
};

} // namespace cli
} // namespace fairyfly

