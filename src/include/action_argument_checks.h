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
    Result result;
    result.status = Result::Status::Error;
    result.error["code"] = "INVALID_VKEY";
    result.error["message"] = fmt::format("Unknown key '{}'", key);
    result.error["suggestions"] = nlohmann::json::array({
        "Use enter, f1..f12, shift+f1..shift+f12, or a raw SAP VKey number (0-99)"});
    return result;
}

/// Structured GRID_ROW_OPTIONS_REQUIRED error when --doubleclick lacks a valid --row/--column.
inline std::optional<Result> check_doubleclick_options(bool doubleclick, std::optional<int> row,
                                                       const std::string& column) {
    if (!doubleclick || (row.has_value() && *row >= 0 && !column.empty())) return std::nullopt;
    Result result;
    result.status = Result::Status::Error;
    result.error["code"] = "GRID_ROW_OPTIONS_REQUIRED";
    result.error["message"] = "--doubleclick requires --row >= 0 and --column on a GridView element";
    return result;
}

} // namespace fairyfly::sap
