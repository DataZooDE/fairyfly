#pragma once

#include <algorithm>
#include <set>
#include <string>

#include "include/mcp/types.h"

namespace fairyfly::mcp {

/// Endpoint grant intersected with the caller's exact SAP identity grant. Existing unbound
/// tokens remain usable on a one-identity endpoint; a multi-identity endpoint requires an
/// explicit token grant so an older token cannot suddenly see another SAP user.
inline bool owner_identity_allowed(const Policy& policy, const Principal& principal, const std::string& identity) {
    if (std::find(policy.owner_sap_identities.begin(), policy.owner_sap_identities.end(), identity) ==
        policy.owner_sap_identities.end()) return false;
    if (principal.sap_identities.empty()) return policy.owner_sap_identities.size() == 1;
    return std::find(principal.sap_identities.begin(), principal.sap_identities.end(), identity) !=
        principal.sap_identities.end();
}

inline bool owner_token_bound(const Policy& policy, const Principal& principal) {
    return policy.owner_sap_identities.size() <= 1 || !principal.sap_identities.empty();
}

/// Tools that may select tabs, navigate or change the saved session.
inline const std::set<std::string>& lease_capable_tools() {
    static const std::set<std::string> names = {
        "gui_element_click", "gui_element_fill", "gui_element_f4", "gui_element_get",
        "gui_key_send", "gui_menu_select", "gui_popup_close", "gui_screen_read",
        "gui_transaction_start", "gui_session_disconnect"};
    return names;
}

/// Called after catalog argument validation. A regular read without tab selection is observational.
inline bool lease_required_for_call(const std::string& tool, const json& args) {
    if (!lease_capable_tools().count(tool)) return false;
    if (tool == "gui_element_get") return args.value("activate_tab", false);
    if (tool == "gui_screen_read") return !args.value("no_tabs", false);
    return true;
}

} // namespace fairyfly::mcp
