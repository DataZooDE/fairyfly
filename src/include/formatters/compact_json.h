#pragma once

#include <nlohmann/json.hpp>

namespace fairyfly {
namespace formatters {

/// Pure transformation behind `screen read --compact` for JSON / TOON output.
///
/// Input is the `data` object of a screen read result. Output:
///  - `hierarchy.<group>` and `tabs_content[].hierarchy.<group>` become arrays of element ids
///    (the element objects live only in `elements`).
///  - `tabs_content[].elements` becomes an array of element ids; the objects live in the
///    top-level `elements` (which already contains every tab element). Any referenced element
///    missing from the top-level `elements` is appended there, so every id always resolves.
///  - In element objects (and nested `children` / `toolbar_buttons` elements) keys whose value is
///    an empty string, null, empty array, or boolean false are omitted. `id`, `type`, `name`
///    are always kept. An absent boolean therefore means false.
///  - Adds `compact: true`, `hierarchy_format: "ids"`, `tab_elements_format: "ids"`.
/// Idempotent: already-compact data is returned unchanged.
///  - Hides grid/table columns that are empty in every returned row (hide_empty_columns).
nlohmann::json compact_screen_json(const nlohmann::json& data);

/// `--compact` for grids and table controls (every format): in each `table_data` / `grid_data` object anywhere in
/// `data`, the columns whose cells are empty (or only white space, or missing) in every returned row are removed
/// from `columns` and every row, and their names are listed in `hidden_empty_columns`. Nothing is hidden without
/// rows or when every column is empty. Idempotent.
void hide_empty_columns(nlohmann::json& data);

}  // namespace formatters
}  // namespace fairyfly
