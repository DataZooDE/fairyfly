#pragma once

#include "grid_types.h"
#include <nlohmann/json.hpp>
#include <vector>
#include <string>

namespace fairyfly {
namespace cli {

/// Analyzes grid layouts in SAP GUI screens
/// Detects grid-based monitoring screens and identifies sections
class GridAnalyzer {
public:
    /// Detect if elements contain grid coordinates
    static bool detect_grid_layout(const nlohmann::json& elements);

    /// Parse grid labels from elements into a 2D structure
    static std::vector<GridCell> parse_grid_labels(const nlohmann::json& elements);

    /// Identify grid sections (status pairs vs rotated tables)
    static std::vector<GridSection> identify_grid_sections(const std::vector<GridCell>& cells);

    /// Deduplicate grid cells by ID
    static std::vector<GridCell> deduplicate_cells(const std::vector<GridCell>& cells);
};

} // namespace cli
} // namespace fairyfly

