// Persisted enrolment of this desktop. The device private key is protected at rest with DPAPI on Windows.
#pragma once
#include <string>
#include <vector>

#include "crypto.hpp"

namespace ffy::link {

struct State {
    std::string link_url, device_id, issuer, tenant, email, tenant_name;
    std::string provider, idp_directory;  // who signed the approving person in (shown before saving, not used afterwards)
    bool email_verified = false;
    std::vector<Jwk> gateway_keys;  // pinned at enrolment
    PKey key;                       // device private key
};

std::string default_state_path();
void save_state(const State& s, const std::string& path);
State load_state(const std::string& path);  // throws ffy::Error

// OS-level protection of the key blob (DPAPI on Windows; identity elsewhere - the file is chmod 0600).
std::string protect(const std::string& plain);
std::string unprotect(const std::string& blob);

}  // namespace ffy::link
