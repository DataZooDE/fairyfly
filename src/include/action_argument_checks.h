#pragma once

#include "core.h"
#include "vkey.h"
#include <fmt/format.h>
#include <optional>
#include <string>

namespace fairyfly::sap {

/// Structured INVALID_VKEY error for an unknown send-key key name, or nullopt when valid.
inline std::optional<Result> check_vkey_argument(const std::string& key) {
    if (parse_vkey(key)) return std::nullopt;
    return Result::fail("INVALID_VKEY", fmt::format("Unknown key '{}'", key),
                        nlohmann::json::array({"Supported keys: " + supported_key_names_text()}));
}

/// Structured GRID_ROW_OPTIONS_REQUIRED error when --doubleclick lacks a valid --row/--column.
inline std::optional<Result> check_doubleclick_options(bool doubleclick, std::optional<int> row,
                                                       const std::string& column) {
    if (!doubleclick || (row.has_value() && *row >= 0 && !column.empty())) return std::nullopt;
    return Result::fail("GRID_ROW_OPTIONS_REQUIRED", "--doubleclick requires --row >= 0 and --column on a GridView element");
}

} // namespace fairyfly::sap
