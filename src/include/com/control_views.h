#pragma once
// Typed views over a ComGuiElement (the element stays the owner): the grid methods of a GuiGridView / grid shell and
// the toolbar methods of a toolbar shell or a grid's built-in toolbar. The tree methods: tree_view.h.
#include <initializer_list>
#include <string>
#include <vector>

#include "include/com/wrapper.h"

namespace fairyfly {
namespace sap {

/// Cells and columns of a GuiGridView (or a grid shell, a table control for its columns).
class ComGuiGrid {
public:
    explicit ComGuiGrid(ComGuiElement& element) : element_(element), dispatch_(element.get_com_object()) {}

    /// Get cell value from GridView
    /// @param row Row index (0-based)
    /// @param column_name Column identifier from ColumnOrder
    /// @return Cell value as string, empty if not available
    std::string get_cell_value(int row, const std::string& column_name) const;
    /// Select a GridView row and activate its cell (zero-based row, technical column ID).
    void select_grid_row(int row, const std::string& column_name);
    /// Double-click a GridView cell (SetCurrentCell then DoubleClickCurrentCell).
    void doubleclick_grid_cell(int row, const std::string& column_name);
    /// Modify a GridView cell and optionally notify the backend after a batch.
    void modify_grid_cell(int row, const std::string& column_name,
                          const std::string& value, bool checkbox, bool commit);
    /// Get column names from ColumnOrder collection
    /// Returns empty vector if no columns available
    std::vector<std::string> get_column_order() const;

private:
    ComGuiElement& element_;
    IDispatch* dispatch_;
};

inline ComGuiGrid as_grid(ComGuiElement& element) { return ComGuiGrid(element); }

/// The buttons of a toolbar shell or a GridView's built-in toolbar, by position.
class ComGuiToolbar {
public:
    explicit ComGuiToolbar(const ComGuiElement& element) : element_(element), dispatch_(element.get_com_object()) {}

    /// Get number of toolbar buttons in GuiShell Toolbar
    /// Returns 0 if not a toolbar or property not available
    int get_button_count() const;
    /// Get button ID at specified position (0-based)
    /// Returns empty string if position invalid or method fails
    std::string get_button_id(int position) const;
    /// One of the toolbar's by-position string getters (GetButtonId, GetButtonText, ...); "" on failure.
    std::string button_string(const wchar_t* method, int position) const;
    /// Get button text at specified position
    /// Returns empty string if position invalid or method fails
    std::string get_button_text(int position) const;
    /// Get button tooltip at specified position
    /// Returns empty string if position invalid or method fails
    std::string get_button_tooltip(int position) const;
    /// Get button type at specified position
    /// Returns type string: "Button", "ButtonAndMenu", "Menu", "Separator", "Group", "CheckBox"
    /// Returns empty string if position invalid or method fails
    std::string get_button_type(int position) const;
    /// Get button enabled state at specified position
    /// Returns false if position invalid or method fails
    bool get_button_enabled(int position) const;
    /// Look up a toolbar button (GuiShell toolbar, then GridView toolbar) by its ID and return its
    /// text and tooltip. Returns false when the button cannot be found or the lookup fails.
    bool find_toolbar_button_labels(const std::string& button_id, std::string& text,
                                    std::string& tooltip) const;
    /// Press a toolbar button by ID (for GuiShell Toolbar elements)
    /// @param button_id Button identifier (e.g., "TECH", "PERF")
    /// Throws ComException if not a toolbar or button not found
    void press_button(const std::string& button_id);
    /// GridView built-in toolbar metadata (separate from GuiShell Toolbar buttons).
    int get_grid_toolbar_button_count() const;
    std::string get_grid_toolbar_button_id(int position) const;
    std::string get_grid_toolbar_button_tooltip(int position) const;

private:
    const ComGuiElement& element_;   // only const members: reading and pressing go through call_method
    IDispatch* dispatch_;
};

inline ComGuiToolbar as_toolbar(const ComGuiElement& element) { return ComGuiToolbar(element); }

} // namespace sap
} // namespace fairyfly
