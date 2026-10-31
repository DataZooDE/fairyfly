#pragma once
#include <string>
#include <vector>

namespace fairyfly::cli {

/// The `only` selectors of a screen read, the same for `screen read --only` and gui_screen_read `only`: buttons,
/// fields, editable, f4_fields, tables (src/screen_filter.cpp; select_only in cli_handler.h sets one).
const std::vector<std::string>& only_selector_values();

}  // namespace fairyfly::cli
