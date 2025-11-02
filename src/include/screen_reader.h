#pragma once

#include "core.h"
#include "com/wrapper.h"
#include "element_metadata_extractor.h"
#include "screen_element_collector.h"
#include <memory>

namespace fairyfly {
namespace sap {

/// Handles screen reading and element discovery
/// Extracted from ComAutomationEngine to improve separation of concerns
class ScreenReader {
public:
    /// Constructor - takes session for element discovery
    explicit ScreenReader(ComGuiSessionPtr session);

    /// Read screen structure and elements
    Result read(bool include_structure = true);

    /// Read screen with all tabs expanded
    Result read_with_tabs();

private:
    ComGuiSessionPtr session_;

    /// Discover all UI elements from a window using recursive traversal
    std::vector<ComGuiElementPtr> discover_elements(ComGuiWindowPtr window);

    /// Recursively traverse element tree and collect unique elements
    void traverse_element_tree(
        ComGuiElementPtr element,
        ScreenElementCollector& collector,
        int depth = 0);

    /// Group elements by container type for better organization
    static json group_elements_by_container(const json& elements);

    /// Recursively flatten and group elements by container type
    static void flatten_and_group_elements(const json& elements, json& grouped);
};

} // namespace sap
} // namespace fairyfly

