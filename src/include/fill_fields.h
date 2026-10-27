#pragma once

#include <nlohmann/json.hpp>

#include <optional>
#include <limits>
#include <string>
#include <vector>

namespace fairyfly::sap {

/// One entry of a multi-field fill (`element fill --fields`, MCP `gui_element_fill` `fields`): the same targets as a
/// single fill, a plain input field or a GridView cell.
struct FillField {
    std::string element;
    std::string value;
    bool clear = false;
    std::optional<int> row;
    std::string column;
    bool checkbox = false;
    bool commit = false;
};

constexpr std::size_t kMaxFillFields = 50;
constexpr long long kMaxFillListRow = 1000000;   ///< the highest row of a fields list entry

/// One field object (element | id | element_id, value | clear, row + column, checkbox, commit): the rules every fill
/// follows, from the CLI, MCP or a fields list. nullopt with `why` when it breaks one.
/// `max_row`: a fields list caps rows at kMaxFillListRow; a single fill takes any row an int holds.
std::optional<FillField> parse_fill_field(const nlohmann::json& field, std::string& why,
                                          long long max_row = std::numeric_limits<int>::max());

/// Parses and validates a fields array (1..kMaxFillFields objects of element, value | clear, row + column, checkbox,
/// commit; `id` / `element_id` are aliases of `element`). On error returns nullopt and sets `why`.
std::optional<std::vector<FillField>> parse_fill_fields(const nlohmann::json& fields, std::string& why);

/// Keys a multi-field fill may press after all fields succeeded: only the ones that stay on the screen's own flow
/// (Enter, F8 Execute). Save, Back and Cancel stay separate, deliberate calls. Returns the VKey or nullopt.
std::optional<int> fill_then_vkey(const std::string& key);

}  // namespace fairyfly::sap
