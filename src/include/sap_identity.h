#pragma once
// An exact SAP identity is written "SID/CLIENT/USER" everywhere (owner-mode endpoints, token grants, session
// workers, the doctor). These helpers are the one place that spells it.
#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace fairyfly {

/// "SID/CLIENT/USER" of anything with system, client and user (audit::SapFacts and friends).
template <class Facts>
std::string sap_identity(const Facts& facts) {
    return facts.system + "/" + facts.client + "/" + facts.user;
}

/// System, client and user are all known (before logon the user is not).
template <class Facts>
bool sap_identity_known(const Facts& facts) {
    return !facts.system.empty() && !facts.client.empty() && !facts.user.empty();
}

/// `identity` is in the list, exactly (the lists are stored normalized).
inline bool identity_listed(const std::vector<std::string>& identities, std::string_view identity) {
    return std::find(identities.begin(), identities.end(), identity) != identities.end();
}

}  // namespace fairyfly
