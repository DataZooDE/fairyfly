#pragma once

#include "include/grid_types.h"
#include <nlohmann/json.hpp>
#include <vector>
#include <string>

namespace fairyfly {
namespace cli {

/// Renders grid layouts as markdown
class GridRenderer {
public:
    /// Render status section as key-value pairs
    static std::string render_status_section(const std::vector<GridCell>& cells);

    /// Render rotated table section
    static std::string render_rotated_table(const std::vector<GridCell>& cells);
};

} // namespace cli
} // namespace fairyfly

