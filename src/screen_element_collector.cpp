#include "include/screen_element_collector.h"

namespace fairyfly {
namespace sap {

bool ScreenElementCollector::add(ComGuiElementPtr element) {
    if (!element) {
        return false;  // Null pointer, skip
    }

    std::string id = element->get_id();

    // O(1) duplicate check using hash set
    if (seen_ids_.find(id) != seen_ids_.end()) {
        return false;  // Already collected
    }

    // Add to both containers
    seen_ids_.insert(id);
    elements_.push_back(element);

    return true;
}

void ScreenElementCollector::clear() {
    elements_.clear();
    seen_ids_.clear();
}

void ScreenElementCollector::reserve(size_t capacity) {
    elements_.reserve(capacity);
    seen_ids_.reserve(capacity);
}

} // namespace sap
} // namespace fairyfly
