#pragma once

#include <string>
#include <vector>
#include <unordered_set>
#include "com_wrapper.h"

namespace fairyfly {
namespace sap {

/// Collects UI elements from SAP GUI screens with O(1) deduplication
/// Replaces the previous O(n²) linear search approach
class ScreenElementCollector {
private:
    std::vector<ComGuiElementPtr> elements_;
    std::unordered_set<std::string> seen_ids_;

public:
    ScreenElementCollector() = default;

    /// Add element if not already collected (O(1) lookup)
    /// @param element The SAP GUI element to add
    /// @return true if element was added, false if it was a duplicate
    bool add(ComGuiElementPtr element);

    /// Get all collected elements
    const std::vector<ComGuiElementPtr>& elements() const { return elements_; }

    /// Get count of collected elements
    size_t size() const { return elements_.size(); }

    /// Clear all collected elements
    void clear();

    /// Reserve capacity for expected number of elements (performance optimization)
    void reserve(size_t capacity);
};

} // namespace sap
} // namespace fairyfly
