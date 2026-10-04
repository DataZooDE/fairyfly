#include "include/tab_guard.h"

namespace fairyfly::sap {

std::vector<TabPageRef> tab_pages_in_path(const std::string& element_id) {
    std::vector<TabPageRef> pages;
    std::vector<std::string> segments;
    size_t start = 0;
    while (start <= element_id.size()) {
        const size_t slash = element_id.find('/', start);
        const size_t end = slash == std::string::npos ? element_id.size() : slash;
        segments.push_back(element_id.substr(start, end - start));
        if (slash == std::string::npos) break;
        start = slash + 1;
    }
    std::string prefix;
    for (size_t i = 0; i < segments.size(); ++i) {
        const std::string& segment = segments[i];
        const bool is_last = i + 1 == segments.size();
        if (i > 0 && !is_last && segment.rfind("tabp", 0) == 0 && segment.size() > 4 &&
            segments[i - 1].rfind("tabs", 0) == 0 && segments[i - 1].size() > 4) {
            pages.push_back({prefix, prefix + "/" + segment, segment});
        }
        prefix = i == 0 ? segment : prefix + "/" + segment;
    }
    return pages;
}

std::optional<std::pair<TabPageRef, TabPageState>> find_inactive_tab_page(const std::string& element_id,
                                                                          const TabPageLookup& lookup) {
    for (const auto& page : tab_pages_in_path(element_id)) {
        const TabPageState state = lookup(page);
        if (state.found && !state.selected) return std::make_pair(page, state);
    }
    return std::nullopt;
}

bool TabActivationGuard::restore() {
    if (!pending_) return restored_;
    pending_ = false;
    restored_ = true;
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it) {
        if (it->previous_id.empty()) {
            restored_ = false;
            if (restore_error_.empty())
                restore_error_ = "the previously selected tab of the strip of " + it->tab_id + " is unknown";
            continue;
        }
        try {
            if (!select_ || !select_(it->previous_id)) {
                restored_ = false;
                if (restore_error_.empty()) restore_error_ = "the previous tab " + it->previous_id + " could not be found";
            }
        } catch (const std::exception& e) {
            restored_ = false;
            if (restore_error_.empty()) restore_error_ = std::string("selecting the previous tab failed: ") + e.what();
        } catch (...) {
            restored_ = false;
            if (restore_error_.empty()) restore_error_ = "selecting the previous tab failed";
        }
    }
    return restored_;
}

std::string tab_read_name(const TabPageRef& page, const TabPageState& state) {
    if (!state.text.empty()) return state.text;
    return page.page_name.size() > 4 ? page.page_name.substr(4) : page.page_name;
}

std::optional<Result> classify_element_on_inactive_tab(const std::string& element_id,
                                                       const TabPageLookup& lookup,
                                                       bool offer_activate_tab) {
    auto inactive = find_inactive_tab_page(element_id, lookup);
    if (!inactive) return std::nullopt;
    const auto& [page, state] = *inactive;
    const std::string name = tab_read_name(page, state);
    // `screen read --tab` takes the tab id or its trailing part (tabpLOGO), not the tab text.
    const auto last_slash = page.page_id.find_last_of('/');
    const std::string tab_arg = last_slash == std::string::npos ? page.page_id : page.page_id.substr(last_slash + 1);

    Result result;
    result.status = Result::Status::Error;
    result.error["code"] = "ELEMENT_ON_INACTIVE_TAB";
    result.error["message"] = "Element " + element_id + " is on the tab '" + name +
                              "', which is not the selected tab of its tab strip";
    result.error["element"] = element_id;
    result.error["tab_id"] = page.page_id;
    result.error["tab_text"] = state.text;
    result.error["tab_strip_id"] = page.strip_id;
    result.error["hint"] = "activate the tab first (gui_element_click on the tab) or read it with gui_screen_read tab=" + tab_arg;
    json suggestions = json::array({
        "Activate the tab first: fairyfly element click '" + page.page_id + "'",
        "Or read it without leaving the current tab: fairyfly screen read --tab '" + tab_arg + "'"});
    if (offer_activate_tab)
        suggestions.push_back("Or repeat the call with --activate-tab (activate_tab=true) to select the tab for the read");
    result.error["suggestions"] = suggestions;
    return result;
}

} // namespace fairyfly::sap
