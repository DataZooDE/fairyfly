#pragma once

#include "core.h"
#include "com/wrapper.h"
#include <memory>

namespace fairyfly {
namespace sap {

/// Extracts rich metadata from SAP GUI elements. One extractor belongs to one screen read: its options and its
/// table/tree cache never outlive that read (ScreenReader makes a fresh one per read).
class ElementMetadataExtractor {
public:
    struct Options {
        bool skip_trees = false;  ///< --skip-trees: do not extract tree controls (workaround for problematic trees)
    };

    explicit ElementMetadataExtractor(Options options = {}) : options_(options) {}

    /// Extract metadata from an element recursively
    json extract(ComGuiElementPtr elem, int depth = 0);

    /// Forget the table/tree data read so far (another tab was selected)
    void clear_cache() { tree_grid_cache_.clear(); }

    const Options& options() const { return options_; }
    std::size_t cached_entries() const { return tree_grid_cache_.size(); }

private:
    // the parts of extract (element_metadata_extractor.cpp)
    void extract_table_or_tree(const ComGuiElementPtr& elem, const std::string& elem_id, const std::string& type,
                               const std::string& text, bool probe_grid, json& metadata);
    static void read_abap_editor(const ComGuiElementPtr& elem, json& metadata);
    static void list_child_ids(const ComGuiElementPtr& elem, const std::string& type, const std::string& container_type,
                               int depth, json& metadata);

    Options options_;
    std::map<std::string, json> tree_grid_cache_;  ///< element id -> extracted table/tree data
};

} // namespace sap
} // namespace fairyfly

