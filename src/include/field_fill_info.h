#pragma once

#include "core.h"
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace fairyfly::sap {

/// What the fill path knows about a text input after typing into it. Plain data so the shaping below is
/// unit-testable without a COM object.
struct FieldProbe {
    std::string type;          ///< GuiTextField, GuiCTextField, GuiComboBox, ...
    std::string id;            ///< full element id
    std::string name;          ///< control name (e.g. ctxtP_DATUM)
    std::string label;         ///< associated label text, may be empty
    std::string text_before;   ///< value shown before the fill (may be empty)
    std::string text_after;    ///< value read back from the control after the fill
    bool text_after_known = false;
    int max_length = 0;        ///< 0 = unknown
    bool numerical = false;
    bool required = false;
};

constexpr size_t kFillEchoMaxChars = 200;

/// "date", "time", "numeric" or "" (plain text / unknown). Heuristic: the scripting API exposes no DDIC type, so
/// the control name, label, MaxLength and the shape of the displayed value are used.
std::string infer_input_kind(const FieldProbe& probe);

/// SAP date formats by the shape of a displayed or typed date: "DD.MM.YYYY", "MM/DD/YYYY", "MM-DD-YYYY",
/// "YYYY.MM.DD", "YYYY/MM/DD", "YYYY-MM-DD". Empty when the text is not date-shaped. Time: "HH:MM:SS".
std::string date_format_of_text(const std::string& text);
std::string time_format_of_text(const std::string& text);

/// The `value` of a fill result: the typed value as read back from the control (truncated to 200 characters, with
/// an ellipsis marker) or "[REDACTED: reason]" for password/credential fields. `redacted` reports which one.
std::string fill_value_echo(const FieldProbe& probe, const std::string& typed, bool& redacted);

/// What a GridView cell fill knows after ModifyCell: the filled column, the grid's column order and the row's cells
/// as read back (GetCellValue), the filled cell included. Plain data so the echo is unit-testable without COM.
struct GridCellProbe {
    std::string column;                       ///< technical name of the filled column (MATNR, MENGE, ...)
    std::optional<std::string> column_title;  ///< its displayed title, when the grid exposes one
    std::vector<std::string> columns;         ///< the row's columns as read back (the filled one included)
    std::vector<std::string> row;             ///< their cells in `columns` order
    bool row_known = false;                   ///< false when a needed cell could not be read or the row moved
};

/// The `value` of a grid cell fill: the cell as read back from the grid (the typed value when the column is not in
/// `columns`), redacted by the rules a screen read of that row applies (redact_sensitive_header_rows: a credential
/// column, a NAME/VALUE row naming a credential) plus a credential-named filled column or displayed title, and
/// masked like a field echo. Not echoed when the row could not be read (fail closed). `redacted` reports a marker.
std::string grid_fill_value_echo(const GridCellProbe& probe, const std::string& typed, bool& redacted);

/// The grid as fill_grid_cell sees it: COM in production (ComGuiGrid), plain functions so the whole flow is
/// unit-testable. `cell` answers nullopt when GetCellValue fails; `title` nullopt when the grid shows no title.
struct GridAccess {
    std::function<std::vector<std::string>()> column_order;
    std::function<std::optional<std::string>(int row, const std::string& column)> cell;
    std::function<std::optional<std::string>(const std::string& column)> title;
    std::function<void()> modify;  ///< the edit itself (ModifyCell / ModifyCheckBox, TriggerModified); may throw
};

struct GridFillEcho {
    std::string value;
    bool redacted = true;
};

/// fill_grid_cell's flow: read the row's anchor (the first non-empty other cell), run the edit, read the whole row
/// once with the filled cell's title, and decide the echo by the screen-read row rule. A failed read, no column order
/// or a changed anchor (the grid re-sorted) give the "could not verify" marker. Exceptions of `modify` propagate.
GridFillEcho fill_grid_cell_echo(const GridAccess& grid, int row, const std::string& column, const std::string& typed);

/// The `field` object of a fill result: {type, max_length?, input_kind?, format_hint?, format_hint_source?,
/// numerical?, required?, value_normalized?, format_warning?}. Never a hard validation: a mismatch with SAP's own
/// normalisation (format_hint_source "normalized_input") only adds
/// format_warning. For credential fields nothing but the type is reported.
json build_fill_field_info(const FieldProbe& probe, const std::string& typed);

/// Controls whose caption is often an icon: element get / screen find read their tooltip as `screen read` does.
bool type_shows_tooltip(const std::string& type);

/// Adds data["tooltip"] when non-empty (absent otherwise) and, when `text` is non-empty and differs from the
/// already reported data["value"] (get) / data["text"] (find), data["text"] as well. No-op for other types.
void attach_tooltip_fields(json& data, const std::string& type, const std::string& tooltip,
                           const std::string& text);

} // namespace fairyfly::sap
