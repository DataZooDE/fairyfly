#pragma once

#include "core.h"
#include "com/wrapper.h"
#include "element_metadata_extractor.h"
#include "screen_element_collector.h"
#include <memory>
#include <functional>
#include <tuple>
#include <vector>
#include <string>

namespace fairyfly {
namespace sap {

enum class ShellExtractionKind { Grid, Tree, Metadata };

inline ShellExtractionKind classify_shell_extraction(const std::string& subtype) {
    if (subtype == "GridView") return ShellExtractionKind::Grid;
    if (subtype == "AbapEditor" || subtype == "TextEdit" ||
        subtype == "HTMLViewer")
        return ShellExtractionKind::Metadata;
    return ShellExtractionKind::Tree;
}

struct ScreenFindOptions {
    std::string id_contains;
    std::string name_contains;
    std::string type;
    int limit = 1;
};

bool screen_candidate_matches(const ScreenFindOptions& query,
                              const std::string& id,
                              const std::string& name,
                              const std::string& type);

struct TabSelectionSnapshot {
    std::vector<std::string> selected_ids;
    static TabSelectionSnapshot capture(
        const json& tabs,
        const std::function<std::string(const std::string&)>& selected_for_strip);
    bool restore(const std::function<void(const std::string&)>& select_tab) const;
};

struct ScreenSearchContext;

/// Handles screen reading and element discovery
/// Extracted from ComAutomationEngine to improve separation of concerns
class ScreenReader {
public:
    /// Constructor - takes session for element discovery
    explicit ScreenReader(ComGuiSessionPtr session);

    /// Read screen structure and elements
    Result read(bool include_structure = true, bool skip_trees = false, int max_rows = 20);

    /// Read screen with all tabs expanded
    Result read_with_tabs(bool skip_trees = false, int max_rows = 20);

    /// Search current visible controls without extracting unrelated values or grid rows.
    Result find(const ScreenFindOptions& query);

    /// A positioned-label user area is a table only when data rows share
    /// multiple columns with its header. Reports also use /lbl[col,row].
    static bool is_tabular_userarea(
        const std::vector<std::tuple<int, int, std::string>>& cells);

    /// Limit returned positioned-label rows while retaining the full visible count.
    static void limit_userarea_table_rows(json& table, int max_rows);

    /// Extract grid data from a GuiGridView/GuiTableControl using fresh COM pointer
    /// @param element Fresh COM pointer to grid element (obtained via session->find_element_by_id)
    /// @param elem_id The element ID
    /// @return JSON object with grid data: {id, type, grid_data: {columns, rows, ...}}
    json extract_grid_data_immediately(ComGuiElementPtr element, const std::string& elem_id);

private:
    ComGuiSessionPtr session_;
    int max_rows_ = 20;

    /// Discover all UI elements from a window using recursive traversal
    /// Populates the provided collector with elements and extracted tree data
    void discover_elements(ComGuiWindowPtr window, ScreenElementCollector& collector,
                           bool skip_trees = false, ScreenSearchContext* search = nullptr);

    /// Recursively traverse element tree and collect unique elements
    void traverse_element_tree(
        ComGuiElementPtr element,
        ScreenElementCollector& collector,
        int depth = 0,
        bool skip_trees = false,
        ScreenSearchContext* search = nullptr);

    /// Extract tree data immediately using safe tree APIs (for GuiShell elements)
    /// This avoids storing COM pointers that become stale and cause segfaults
    /// @param element The GuiShell tree element
    /// @param elem_id The element ID
    /// @return JSON object with tree data: {id, type, subtype, tree_nodes: [...]}
    json extract_tree_data_immediately(ComGuiElementPtr element, const std::string& elem_id);


    /// Extract grid data from a GuiUserArea with positioned labels (lbl[row,col])
    /// @param element Fresh COM pointer to GuiUserArea element
    /// @param elem_id The GuiUserArea element ID
    /// @param collector Element collector containing grid cell coordinates
    /// @return JSON object with grid data: {id, type, grid_data: {rows, row_count, col_count}}
    json extract_userarea_grid_data(ComGuiElementPtr element, const std::string& elem_id,
                                    const ScreenElementCollector& collector);

    /// Extract metadata for all elements collected in collector
    json extract_metadata_for_collector(const ScreenElementCollector& collector);

    /// Group elements by container type for better organization
    static json group_elements_by_container(const json& elements);

    /// Recursively flatten and group elements by container type
    static void flatten_and_group_elements(const json& elements, json& grouped);
};

} // namespace sap
} // namespace fairyfly

