#pragma once

#include "bulk_screen_reader.h"
#include "core.h"
#include "com/wrapper.h"
#include "element_metadata_extractor.h"
#include "screen_element_collector.h"
#include <memory>
#include <functional>
#include <optional>
#include <tuple>
#include <vector>
#include <string>

namespace fairyfly {
namespace sap {

enum class ShellExtractionKind { Grid, Tree, Metadata };

inline ShellExtractionKind classify_shell_extraction(const std::string& subtype) {
    if (subtype == "GridView") return ShellExtractionKind::Grid;
    if (subtype == "Tree" || subtype == "TableTreeControl")
        return ShellExtractionKind::Tree;
    // Everything else (Toolbar, HTMLViewer, AbapEditor, TextEdit, Calendar,
    // unknown or empty SubType) goes through the metadata probe so its
    // content is not silently dropped (ERR-137).
    return ShellExtractionKind::Metadata;
}

/// Whether `screen find` can report a simple text value for this element type.
inline bool find_type_has_simple_text(const std::string& type) {
    return type == "GuiButton" || type == "GuiTextField" ||
           type == "GuiCTextField" || type == "GuiPasswordField" ||
           type == "GuiLabel" || type == "GuiStatusbar" ||
           type == "GuiCheckBox" || type == "GuiRadioButton" ||
           type == "GuiComboBox" || type == "GuiOkCodeField" ||
           type == "GuiTitlebar" || type == "GuiTab";
}

struct ScreenFindOptions {
    std::string id_contains;
    std::string name_contains;
    std::string type;
    int limit = 1;
    bool probe_all = false;  // --probe-all: exhaustive FindById probing (see id_probe_candidates)
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

/// Whether selecting a tab is needed given the strip's currently selected tab.
enum class TabSelectionPlan { AlreadySelected, Select };
inline TabSelectionPlan plan_tab_selection(const std::string& selected_id,
                                           const std::string& requested_id) {
    return (!selected_id.empty() && selected_id == requested_id)
        ? TabSelectionPlan::AlreadySelected : TabSelectionPlan::Select;
}

/// FindById paths worth probing below a container because Children may not list them.
/// Pure: decides from the control type and the number of children Children enumerated.
///  - Never probed: GuiToolbar, GuiTitlebar, GuiMenubar, GuiTabStrip, GuiBox,
///    GuiStatusbar, GuiUserArea (their Children collection is complete; probing them
///    only produced misses).
///  - /shell, /shell[0..3], /shellcont[0..3], /shellcont: shell-hosting controls only
///    (GuiCustomControl, GuiContainerShell, GuiSplitterShell, GuiSplitterContainer,
///    GuiDockShell, GuiContainerCtrl).
///  - /sub[0..3], /ssub<NAME>, /cntl<NAME>, /cntl[0..3]: subscreen-like containers
///    (GuiSimpleContainer, GuiScrollContainer, GuiSubScreen, GuiTab) and only when
///    Children enumeration yielded no children (safety net for SE16/SEGW layouts).
///  - probe_all (`--probe-all` / FAIRYFLY_PROBE_ALL=1): the legacy exhaustive set (every
///    shell and subscreen candidate) for every container regardless of type or children.
std::vector<std::string> id_probe_candidates(const std::string& type,
                                             const std::string& elem_id,
                                             int children_found,
                                             bool probe_all = false);

/// Same, minus candidates the collector already knows.
std::vector<std::string> id_probe_candidates(const std::string& type,
                                             const std::string& elem_id,
                                             int children_found,
                                             const ScreenElementCollector& collector,
                                             bool probe_all = false);

/// True when FAIRYFLY_PROBE_ALL=1 requests exhaustive ID probing.
bool probe_all_from_environment();

/// How `screen read` obtains the element tree: element by element over COM (Legacy), or with one
/// GuiSession.GetObjectTree call (Bulk), the latter with a per-read fallback to Legacy (Auto).
enum class TreeReaderMode { Legacy, Bulk, Auto };

/// FAIRYFLY_SCREEN_READER=legacy|bulk|auto; unset or unknown means Legacy.
TreeReaderMode tree_reader_mode_from_environment();

/// "legacy", "bulk" or "auto" (any case); nullopt for anything else.
std::optional<TreeReaderMode> parse_tree_reader_mode(const std::string& text);
const char* tree_reader_mode_name(TreeReaderMode mode);

/// True once the bulk reader is switched off for the process (RPC_E_SERVERFAULT, or three
/// consecutive validator failures). Thread-safe.
bool bulk_reader_disabled();
/// Re-enables the bulk reader and clears its failure counters (tests).
void reset_bulk_reader_state_for_testing();

/// Why one tab could not be read.
enum class TabReadStatus { Ok, NotFound, BusyTimeout };
const char* tab_read_failure_reason(TabReadStatus status);

/// Test hook: override the post-select busy wait (default 5000 ms); <= 0 restores it.
void set_tab_wait_timeout_ms_for_testing(int ms);

struct ScreenSearchContext;

/// Handles screen reading and element discovery
/// Extracted from ComAutomationEngine to improve separation of concerns
class ScreenReader {
public:
    /// Constructor - takes session for element discovery
    explicit ScreenReader(ComGuiSessionPtr session);

    /// Read screen structure and elements
    Result read(bool include_structure = true, bool skip_trees = false, int max_rows = 20);

    /// Opt in to exhaustive FindById probing of every container (legacy behavior).
    void set_probe_all(bool probe_all) { probe_all_ = probe_all; }

    /// Tree reader mode for subsequent reads; nullopt (default) follows FAIRYFLY_SCREEN_READER.
    void set_tree_reader_mode(std::optional<TreeReaderMode> mode) { tree_reader_mode_ = mode; }

    /// Test seams: replace GuiSession.GetObjectTree and the targeted AccLabel/Selected reads.
    void set_object_tree_source(ObjectTreeSource source) { tree_source_ = std::move(source); }
    void set_element_probe_source(ElementProbeSource source) { probe_source_ = std::move(source); }

    /// False when the caller's filter discards every grid and table (only buttons/fields/...):
    /// their cells are then not read at all. Default true.
    void set_grid_rows_needed(bool needed) { grid_rows_needed_ = needed; }

    /// First grid/table row to return (`--offset`); rows before it are skipped.
    void set_row_offset(int row_offset) { row_offset_ = row_offset < 0 ? 0 : row_offset; }

    /// Read screen with all tabs expanded
    /// When only_tab is non-empty, only the tab whose id equals it (or ends with it at a
    /// '/' boundary) is expanded; TAB_NOT_FOUND is returned when none matches.
    Result read_with_tabs(bool skip_trees = false, int max_rows = 20,
                          const std::string& only_tab = "");

    /// Targeted read of a single tab (`screen read --tab <id>`): locates the owning tab
    /// strip cheaply, selects the tab only if needed, re-fetches it after the wait and
    /// extracts only that tab's subtree (including grids and trees). Restores the
    /// previously selected tab afterwards.
    Result read_tab(const std::string& only_tab, bool skip_trees = false, int max_rows = 20);

    /// Pure tab selection used by `screen read --tab`: an empty selector keeps all tabs.
    static std::vector<json> select_tabs(const std::vector<json>& tabs,
                                         const std::string& only_tab);

    /// Remove selection-screen `%_..._%_APP_%-TEXT` GuiLabels that are empty or repeat
    /// the `label` of a sibling field, so the text is not emitted twice.
    static void collapse_label_duplicates(json& elements);

    /// Search current visible controls without extracting unrelated values or grid rows.
    Result find(const ScreenFindOptions& query);

    /// A positioned-label user area is a table only when data rows share
    /// multiple columns with its header. Reports also use /lbl[col,row].
    static bool is_tabular_userarea(
        const std::vector<std::tuple<int, int, std::string>>& cells);

    /// Limit returned positioned-label rows while retaining the full visible count.
    static void limit_userarea_table_rows(json& table, int max_rows, int offset = 0);

    /// Extract grid data from a GuiGridView/GuiTableControl using fresh COM pointer
    /// @param element Fresh COM pointer to grid element (obtained via session->find_element_by_id)
    /// @param elem_id The element ID
    /// @return JSON object with grid data: {id, type, grid_data: {columns, rows, ...}}
    json extract_grid_data_immediately(ComGuiElementPtr element, const std::string& elem_id);

private:
    ComGuiSessionPtr session_;
    int max_rows_ = 20;
    int row_offset_ = 0;
    bool grid_rows_needed_ = true;
    bool probe_all_ = false;
    std::optional<TreeReaderMode> tree_reader_mode_;
    ObjectTreeSource tree_source_;
    ElementProbeSource probe_source_;

    /// Effective mode of this read: the override, else the environment.
    TreeReaderMode effective_tree_reader_mode() const;

    /// Bulk read of the whole window with one GetObjectTree call: replays discovery over the
    /// parsed tree, runs phase 2/2B and builds the plain elements from it. False (with `reason`)
    /// when this read must use the legacy reader; `elements` is then untouched.
    bool read_elements_bulk(ComGuiWindowPtr window, int window_child_count, bool skip_trees,
                            json& elements, std::string& reason);
    ObjectTreeSource object_tree_source() const;
    ElementProbeSource element_probe_source() const;

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

    /// Phase 2/2B: extract trees and grids collected by traverse_element_tree using
    /// fresh COM pointers.
    void extract_collected_trees_and_grids(ScreenElementCollector& collector);

    /// Select (when needed), wait, re-fetch and read one tab's subtree. Reports NotFound
    /// when the tab is missing and BusyTimeout when the session stays busy after the
    /// select. `elements` is the tab subtree only (not the surrounding window chrome).
    TabReadStatus read_tab_content(const std::string& tab_id, bool needs_select, bool skip_trees,
                          json& elements);

    /// Extract metadata for all elements collected in collector
    /// With a snapshot (bulk path), plain elements the tree describes are built from it without
    /// COM calls; everything else is read element by element as always.
    json extract_metadata_for_collector(const ScreenElementCollector& collector,
                                        const TreeSnapshot* snapshot = nullptr);

    /// Group elements by container type for better organization
    static json group_elements_by_container(const json& elements);

    /// Recursively flatten and group elements by container type
    static void flatten_and_group_elements(const json& elements, json& grouped);
};

} // namespace sap
} // namespace fairyfly

