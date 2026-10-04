#pragma once

#include <string>

#include <nlohmann/json.hpp>

#include "include/audit_log.h"
#include "include/auth/crypto.h"

namespace fairyfly::mcp {

// A conditional-action token for the session and dynpro observed by a client.
// It detects navigation or session replacement, including while a call waits
// in a lane. It is not a fingerprint of every mutable field on the same dynpro.
inline std::string screen_guard_for(const audit::SapFacts& facts) {
    if (!facts.connection_id || facts.session_identity.empty() || facts.system.empty() ||
        facts.client.empty() || facts.user.empty() || facts.transaction.empty() ||
        facts.program.empty() || facts.screen_number.empty()) return {};
    return auth::sha256_hex(nlohmann::json::array({
        *facts.connection_id, facts.session_identity, facts.system, facts.client,
        facts.user, facts.transaction, facts.program, facts.screen_number}).dump());
}

} // namespace fairyfly::mcp
