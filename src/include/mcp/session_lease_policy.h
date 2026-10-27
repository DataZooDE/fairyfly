#pragma once

#include <algorithm>
#include <set>
#include <string>

#include "include/mcp/types.h"

#include "include/sap_identity.h"
#include "include/command_table.h"
namespace fairyfly::mcp {

/// Endpoint grant intersected with the caller's exact SAP identity grant. Existing unbound
/// tokens remain usable on a one-identity endpoint; a multi-identity endpoint requires an
/// explicit token grant so an older token cannot suddenly see another SAP user.
inline bool owner_identity_allowed(const Policy& policy, const Principal& principal, const std::string& identity) {
    if (!identity_listed(policy.owner_sap_identities, identity)) return false;
    if (principal.sap_identities.empty()) return policy.owner_sap_identities.size() == 1;
    return identity_listed(principal.sap_identities, identity);
}

inline bool owner_token_bound(const Policy& policy, const Principal& principal) {
    return policy.owner_sap_identities.size() <= 1 || !principal.sap_identities.empty();
}


/// Called after catalog argument validation. A regular read without tab selection is observational.
inline bool lease_required_for_call(const std::string& tool, const json& args) {
    if (!command_table::tool_has(tool, command_table::Trait::LeaseCapable)) return false;
    if (tool == "gui_element_get") return args.value("activate_tab", false);
    if (tool == "gui_screen_read") return !args.value("no_tabs", false);
    return true;
}

} // namespace fairyfly::mcp
