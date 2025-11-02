#pragma once

#include <string>
#include <vector>

namespace fairyfly {
namespace cli {

/// Represents a single cell in a grid layout
struct GridCell {
    std::string text;
    std::string id;
    int row;
    int col;
};

/// Represents a section of grid cells (status pairs or rotated table)
struct GridSection {
    enum Type { StatusPairs, RotatedTable };
    Type type;
    std::vector<GridCell> cells;
    int start_row;
    int end_row;
    int start_col;
    int end_col;
};

} // namespace cli
} // namespace fairyfly

