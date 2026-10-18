// Short-lived call assertions: created by the gateway (signed with KMS), verified offline by the desktop agent.
#pragma once
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "crypto.hpp"
#include "policy.hpp"
#include "signer.hpp"

namespace ffy::assertion {

inline constexpr int64_t kMaxLifetimeSec = 120;

struct Claims {
    std::string iss, aud, sub, jti, req, client, tenant;
    std::string approval;  // id of a write approval the user gave on the phone with a passkey (D9); empty if none
    int64_t iat = 0, exp = 0;
    policy::Grants grants;
};

std::string sign(Signer& signer, const Claims& c);

class Verifier {
public:
    // tenant: the tenant this device was enrolled into; an assertion naming any other tenant is rejected.
    Verifier(std::string issuer, std::string tenant, const std::vector<Jwk>& pinned_keys);  // throws if keys or tenant empty
    // Verifies signature, issuer, tenant, audience (this device), lifetime, request binding and replay. Throws ffy::Error.
    Claims verify(const std::string& token, const std::string& device_id, const std::string& request_id);
    int64_t (*now_fn)() = &now_unix;

private:
    std::string issuer_, tenant_;
    std::map<std::string, PKey> keys_;  // by recomputed thumbprint (never trust the claimed kid)
    std::mutex mu_;
    std::map<std::string, int64_t> seen_;
};

}  // namespace ffy::assertion
