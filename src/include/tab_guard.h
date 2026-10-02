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

/// A tab page that was activated for a read, and the page that was selected in the same strip before.
struct ActivatedTab {
    std::string tab_id;       ///< the page that was selected for the read
    std::string tab_text;
    std::string previous_id;  ///< the strip's previously selected page ("" = unknown: cannot be restored)
};

/// Scope guard for `--activate-tab`: every activation is recorded BEFORE the page is selected, and the previously selected
/// pages are put back (innermost first) on every exit path: restore() explicitly, or the destructor when an exception
/// unwound the read. A failing restore never throws; it is reported through restored() / restore_error().
class TabActivationGuard {
public:
    /// Selects the page with this id; throws or returns false when that is not possible.
    using SelectPage = std::function<bool(const std::string& page_id)>;
    explicit TabActivationGuard(SelectPage select) : select_(std::move(select)) {}
    TabActivationGuard(const TabActivationGuard&) = delete;
    TabActivationGuard& operator=(const TabActivationGuard&) = delete;
    ~TabActivationGuard() { restore(); }

    /// Records an activation about to happen (call it BEFORE selecting the page).
    void record(ActivatedTab entry) { entries_.push_back(std::move(entry)); pending_ = true; }
    const std::vector<ActivatedTab>& entries() const { return entries_; }

    /// Puts the previous tabs back, once. Returns restored().
    bool restore();
    bool restored() const { return restored_; }
    const std::string& restore_error() const { return restore_error_; }

private:
    SelectPage select_;
    std::vector<ActivatedTab> entries_;
    bool pending_ = false;
    bool restored_ = true;
    std::string restore_error_;
};

} // namespace fairyfly::sap
