#pragma once

#include <algorithm>
#include <string>
#include <vector>
#include <unordered_set>
#include <unordered_map>
#include "com/wrapper.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;

namespace fairyfly {
namespace sap {

/// Collects UI element IDs from SAP GUI screens with O(1) deduplication
/// Stores element IDs (strings) instead of COM pointers to avoid stale pointer issues
/// COM pointers can become invalid during/after traversal, so we fetch fresh ones when needed
class ScreenElementCollector {
private:
    std::vector<std::string> element_ids_;  // Element IDs collected during traversal (safe, never stale)
    std::unordered_set<std::string> seen_ids_;  // For O(1) deduplication
    std::unordered_map<std::string, json> extracted_data_;  // Pre-extracted data for problematic elements
    std::vector<std::string> tree_element_ids_;  // GuiShell IDs found during traversal for Phase 2 extraction
    std::vector<std::string> container_element_ids_;  // GuiUserArea IDs found during traversal for Phase 2 extraction
    std::vector<std::string> grid_element_ids_;  // GuiGridView/GuiTableControl IDs found for Phase 2 extraction
    std::unordered_map<std::string, std::vector<std::tuple<int, int, std::string>>> grid_cells_;  // Grid cell coordinates for GuiUserArea grids
    std::unordered_map<std::string, std::string> known_types_;  // id -> type seen in Phase 1

public:
    ScreenElementCollector() = default;

    /// Add element if not already collected (O(1) lookup)
    /// @param element The SAP GUI element to add (ID will be extracted and stored)
    /// @return true if element was added, false if it was a duplicate
    bool add(ComGuiElementPtr element);

    /// Add an element by ID without touching COM (identity already known)
    /// @return true if the ID was new
    bool add_id(const std::string& id) {
        if (id.empty() || !seen_ids_.insert(id).second) return false;
        element_ids_.push_back(id);
        return true;
    }

    /// Add pre-extracted data for an element (for elements whose COM pointers can't be safely stored)
    /// Used for GuiShell tree controls that cause segfaults when accessed later
    /// @param elem_id The element ID
    /// @param data The pre-extracted JSON data
    void add_extracted_data(const std::string& elem_id, json data);

    /// Add GuiShell tree element ID for Phase 2 extraction (collect during traversal, extract later)
    /// @param elem_id The GuiShell element ID
    void add_tree_id(const std::string& elem_id);

    /// Add GuiUserArea container element ID for Phase 2 extraction (collect during traversal, extract later)
    /// @param elem_id The GuiUserArea element ID
    void add_container_id(const std::string& elem_id);

    /// Add GuiGridView/GuiTableControl element ID for Phase 2 extraction (collect during traversal, extract later)
    /// @param elem_id The grid element ID
    void add_grid_id(const std::string& elem_id);

    /// True when the ID was already seen (collected element, tree, grid or extracted data)
    bool contains(const std::string& id) const { return seen_ids_.find(id) != seen_ids_.end(); }

    /// Remember the element type read during traversal so metadata extraction
    /// does not have to read it again from a fresh COM object.
    void set_known_type(const std::string& id, const std::string& type) { known_types_[id] = type; }

    /// Type recorded by set_known_type, or empty when unknown
    const std::string& known_type(const std::string& id) const {
        static const std::string empty;
        auto it = known_types_.find(id);
        return it == known_types_.end() ? empty : it->second;
    }

    /// Get all collected element IDs (safe - IDs never become stale)
    const std::vector<std::string>& element_ids() const { return element_ids_; }

    /// Get all GuiShell tree IDs found during traversal
    const std::vector<std::string>& get_tree_ids() const { return tree_element_ids_; }

    /// Get all GuiUserArea container IDs found during traversal
    const std::vector<std::string>& get_container_ids() const { return container_element_ids_; }

    /// Get all GuiGridView/GuiTableControl IDs found during traversal
    const std::vector<std::string>& get_grid_ids() const { return grid_element_ids_; }

    /// Check if element ID is an extracted grid ID
    bool is_grid_id(const std::string& elem_id) const {
        return std::find(grid_element_ids_.begin(), grid_element_ids_.end(), elem_id) != grid_element_ids_.end();
    }

    /// Set grid cell coordinates for a GuiUserArea grid (row, col, label_id)
    /// @param grid_id The GuiUserArea element ID containing the grid
    /// @param cells Vector of tuples (row, col, label_id) for each grid cell
    void set_grid_cells(const std::string& grid_id, const std::vector<std::tuple<int, int, std::string>>& cells);

    /// Get grid cell coordinates for a GuiUserArea grid
    /// @param grid_id The GuiUserArea element ID
    /// @return Vector of tuples (row, col, label_id), or empty vector if not found
    const std::vector<std::tuple<int, int, std::string>>& get_grid_cells(const std::string& grid_id) const;

    /// Get all pre-extracted data
    const std::unordered_map<std::string, json>& get_extracted_data() const { return extracted_data_; }

    /// Get count of collected element IDs
    size_t size() const { return element_ids_.size(); }

    /// Get count of pre-extracted elements
    size_t extracted_count() const { return extracted_data_.size(); }

    /// Clear all collected elements
    void clear();

    /// Reserve capacity for expected number of elements (performance optimization)
    void reserve(size_t capacity);
};

} // namespace sap
} // namespace fairyfly
