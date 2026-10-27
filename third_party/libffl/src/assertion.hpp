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
// Clock tolerance between gateway and desktop. The gateway mints iat = now, exp = iat + 60 s, so a desktop clock up to
// 90 s behind (iat in the future) or ahead (exp past) is accepted. Kept short on purpose: on a synced desktop only the
// exp leeway lengthens how long an assertion works in real time (65 s -> 90 s after issue); larger drifts get a
// clear ASSERTION_CLOCK_SKEW with the measured offset instead of a wider window.
inline constexpr int64_t kFutureLeewaySec = 90;
inline constexpr int64_t kExpiryLeewaySec = 30;
// jti replay cache: an entry must outlive the last moment its assertion could still be accepted
inline constexpr int64_t kSeenKeepSec = 300;
static_assert(kSeenKeepSec > kExpiryLeewaySec, "the replay cache must cover the expiry leeway");

// The assertion is genuine (signature, issuer, tenant, audience, request) but the desktop's clock disagrees with the
// gateway's by more than the tolerance. offset_s = gateway time - desktop time: > 0 the desktop is behind.
struct ClockSkew : Error {
    ClockSkew(int64_t offset, const std::string& what) : Error(what), offset_s(offset) {}
    int64_t offset_s;
};
// What the caller and the person at the desktop read: the offset and how to fix it. Never contains token data.
std::string clock_skew_message(int64_t offset_s);

// The assertion is genuine but expired beyond the slack while its iat is not in the future. That fits a desktop clock
// ahead, but just as well a late delivery (a retried or queued frame, a replay), so it is not reported as clock skew.
// late_s: seconds past exp on this desktop's clock; offset_s as for ClockSkew (< 0: the desktop would be ahead).
struct Expired : Error {
    Expired(int64_t late, int64_t offset, const std::string& what) : Error(what), late_s(late), offset_s(offset) {}
    int64_t late_s, offset_s;
};
std::string expired_message(int64_t late_s);

struct Claims {
    std::string iss, aud, sub, jti, req, client, tenant;
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
