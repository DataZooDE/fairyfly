#pragma once

#include "com/wrapper.h"
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

namespace fairyfly::sap {

/// Lower-case ASCII, strip '&' accelerator markers, collapse blanks and drop a trailing ellipsis ("..." or U+2026,
/// which some SAP GUI themes add to entries that open a dialog).
std::string normalize_menu_label(const std::string& label);

/// A label as shown to a caller: '&' accelerator markers removed, blanks trimmed (case and dots kept).
std::string display_menu_label(const std::string& label);

/// Split "Runtime Errors/Display" into path segments (trimmed, empty segments dropped).
std::vector<std::string> split_menu_path(const std::string& path);

/// True when two menu labels are equal after normalize_menu_label.
bool menu_label_matches(const std::string& actual, const std::string& wanted);

/// Enumerate the children of a menu bar or menu as
/// [{id, text, enabled, children:[...]}]. Never selects anything.
/// Text falls back to the control Name when empty. Depth is capped by
/// constants::MAX_ELEMENT_DEPTH.
nlohmann::json read_menu_tree(const ComGuiElementPtr& node, int depth = 0);

/// Up to `max` labels of one menu level closest to `wanted` (normalized edit distance), nearest first; empty when none
/// is close.
std::vector<std::string> closest_menu_labels(const std::string& wanted, const std::vector<std::string>& labels,
                                             size_t max = 3);

/// Where a menu path leads: the item, or the segment that matched nothing and the labels of that level.
struct MenuPathMatch {
    ComGuiElementPtr item;                 ///< nullptr when a segment matched nothing
    size_t failed_index = 0;               ///< the segment that matched nothing (item == nullptr)
    std::vector<std::string> available;    ///< the labels of that level (display_menu_label)
};
MenuPathMatch match_menu_path(const ComGuiElementPtr& root, const std::vector<std::string>& path);

/// Walk from root along a text path and return the matching menu item
/// (nullptr when a segment does not match). Never selects anything.
ComGuiElementPtr find_menu_by_path(const ComGuiElementPtr& root,
                                   const std::vector<std::string>& path);

} // namespace fairyfly::sap
