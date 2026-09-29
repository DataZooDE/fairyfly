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

    // Store ID only (not COM pointer) to avoid stale pointer issues
    // We'll fetch fresh COM pointers via session->find_element_by_id() when needed
    seen_ids_.insert(id);
    element_ids_.push_back(id);

    return true;
}

void ScreenElementCollector::add_extracted_data(const std::string& elem_id, json data) {
    extracted_data_[elem_id] = std::move(data);
    // Also mark as seen to prevent duplicate processing
    seen_ids_.insert(elem_id);
}

void ScreenElementCollector::add_tree_id(const std::string& elem_id) {
    tree_element_ids_.push_back(elem_id);
    // Also mark as seen to prevent duplicate processing
    seen_ids_.insert(elem_id);
}

void ScreenElementCollector::add_container_id(const std::string& elem_id) {
    container_element_ids_.push_back(elem_id);
    // Also mark as seen to prevent duplicate processing
    seen_ids_.insert(elem_id);
}

void ScreenElementCollector::add_grid_id(const std::string& elem_id) {
    grid_element_ids_.push_back(elem_id);
    // Also mark as seen to prevent duplicate processing
    seen_ids_.insert(elem_id);
}

void ScreenElementCollector::set_grid_cells(const std::string& grid_id, const std::vector<std::tuple<int, int, std::string>>& cells) {
    grid_cells_[grid_id] = cells;
}

const std::vector<std::tuple<int, int, std::string>>& ScreenElementCollector::get_grid_cells(const std::string& grid_id) const {
    static const std::vector<std::tuple<int, int, std::string>> empty_vector;
    auto it = grid_cells_.find(grid_id);
    if (it != grid_cells_.end()) {
        return it->second;
    }
    return empty_vector;
}

void ScreenElementCollector::clear() {
    element_ids_.clear();
    seen_ids_.clear();
    extracted_data_.clear();
    tree_element_ids_.clear();
    container_element_ids_.clear();
    grid_element_ids_.clear();
    grid_cells_.clear();
}

void ScreenElementCollector::reserve(size_t capacity) {
    element_ids_.reserve(capacity);
    seen_ids_.reserve(capacity);
    extracted_data_.reserve(capacity / 10);  // Trees are typically <10% of elements
}

} // namespace sap
} // namespace fairyfly
