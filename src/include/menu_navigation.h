#pragma once

#include "com/wrapper.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace fairyfly::sap {

/// Lower-case ASCII, strip '&' accelerator markers and trim blanks.
std::string normalize_menu_label(const std::string& label);

/// Split "Runtime Errors/Display" into path segments (trimmed, empty segments dropped).
std::vector<std::string> split_menu_path(const std::string& path);

/// True when two menu labels are equal after normalize_menu_label.
bool menu_label_matches(const std::string& actual, const std::string& wanted);

/// Enumerate the children of a menu bar or menu as
/// [{id, text, enabled, children:[...]}]. Never selects anything.
/// Text falls back to the control Name when empty. Depth is capped by
/// constants::MAX_ELEMENT_DEPTH.
nlohmann::json read_menu_tree(const ComGuiElementPtr& node, int depth = 0);

/// Walk from root along a text path and return the matching menu item
/// (nullptr when a segment does not match). Never selects anything.
ComGuiElementPtr find_menu_by_path(const ComGuiElementPtr& root,
                                   const std::vector<std::string>& path);

} // namespace fairyfly::sap
