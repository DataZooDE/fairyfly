#pragma once

#include "core.h"
#include "com/wrapper.h"
#include <memory>

namespace fairyfly {
namespace sap {

/// Extracts rich metadata from SAP GUI elements
/// Extracted from ComAutomationEngine to improve separation of concerns
class ElementMetadataExtractor {
public:
    /// Extract metadata from an element recursively
    static json extract(ComGuiElementPtr elem, int depth = 0);

    /// Clear the extraction cache (used for table/tree data)
    static void clear_cache();

    /// Enable/disable tree extraction (workaround for problematic trees)
    static void set_skip_trees(bool skip);
    static bool get_skip_trees();

private:
    static std::map<std::string, json> tree_grid_cache_;
    static bool skip_trees_;
};

} // namespace sap
} // namespace fairyfly

