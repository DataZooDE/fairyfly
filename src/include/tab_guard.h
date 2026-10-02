#pragma once

#include "core.h"
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace fairyfly::sap {

/// One tab page on the way to an element: `.../tabsSTRIP/tabpPAGE/...`.
struct TabPageRef {
    std::string strip_id;  ///< id of the GuiTabStrip (the path up to and excluding the tabp segment)
    std::string page_id;   ///< id of the GuiTab page (strip id + "/tabpPAGE")
    std::string page_name; ///< "tabpPAGE"
};

/// Tab pages the element id passes through, outermost first. A page that IS the id itself is not listed: only the
/// pages the element lives under matter. A `tabp` segment only counts directly after a `tabs` segment.
std::vector<TabPageRef> tab_pages_in_path(const std::string& element_id);

struct TabPageState {
    bool found = false;     ///< the page object exists (inactive pages still do; their content does not)
    bool selected = false;  ///< the strip's SelectedTab is this page
    std::string text;       ///< the tab caption
};

using TabPageLookup = std::function<TabPageState(const TabPageRef&)>;

/// The first tab page of `element_id` that exists but is not the selected one, or nullopt.
std::optional<std::pair<TabPageRef, TabPageState>> find_inactive_tab_page(const std::string& element_id,
                                                                          const TabPageLookup& lookup);

/// ELEMENT_ON_INACTIVE_TAB for an element that is missing because its tab page is not selected:
/// error.{element, tab_id, tab_text, tab_strip_id, hint, suggestions}. nullopt when no tab page is to blame.
std::optional<Result> classify_element_on_inactive_tab(const std::string& element_id,
                                                       const TabPageLookup& lookup,
                                                       bool offer_activate_tab);

/// Tab name to hand to `screen read --tab` (the caption, or the page name without "tabp" when it has none).
std::string tab_read_name(const TabPageRef& page, const TabPageState& state);

} // namespace fairyfly::sap
