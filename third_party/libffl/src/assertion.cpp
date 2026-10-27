#include "assertion.hpp"

#include "jws.hpp"

namespace ffy::assertion {

std::string sign(Signer& s, const Claims& c) {
    nlohmann::json hdr = {{"alg", "ES256"}, {"typ", "ffl+jwt"}, {"kid", s.kid()}};
    nlohmann::json cl = {{"iss", c.iss}, {"aud", c.aud}, {"sub", c.sub}, {"jti", c.jti}, {"iat", c.iat}, {"exp", c.exp},
                         {"req", c.req}, {"client", c.client}, {"tenant", c.tenant}, {"grants", c.grants.to_json()}};
    return jws::encode(hdr, cl, [&](const std::string& in) { return s.sign(in); });
}

std::string clock_skew_message(int64_t offset_s) {
    const int64_t d = offset_s < 0 ? -offset_s : offset_s;
    return "the desktop's clock is " + std::to_string(d) + " s " + (offset_s < 0 ? "ahead of" : "behind") +
           " the gateway's; sync the Windows clock (Settings > Time & language > Date & time > Sync now) and try again";
}

std::string expired_message(int64_t late_s) {
    return "the request reached the desktop too late (" + std::to_string(late_s) +
           " s after its authorization expired); try again, and if this repeats, check the PC's clock";
}

Verifier::Verifier(std::string issuer, std::string tenant, const std::vector<Jwk>& keys) : issuer_(std::move(issuer)), tenant_(std::move(tenant)) {
    if (tenant_.empty()) throw Error("assertion: no tenant in device state (re-enrol)");
    for (auto& k : keys) {
        PKey p = k.ec();
        keys_[thumbprint(p)] = p;
    }
    if (keys_.empty()) throw Error("assertion: no pinned gateway keys");
}

Claims Verifier::verify(const std::string& token, const std::string& device_id, const std::string& request_id) {
    jws::Token t;
    try {
        t = jws::parse(token);
    } catch (const Error&) {
        throw Error("assertion: malformed");
    }
    if (t.str("alg") != "ES256" || t.str("typ") != "ffl+jwt") throw Error("assertion: bad header");
    auto k = keys_.find(t.str("kid"));
    if (k == keys_.end()) throw Error("assertion: unknown kid (re-enrol to refresh pinned keys)");
    if (!es256_verify(k->second, t.input, t.sig)) throw Error("assertion: bad signature");
    nlohmann::json j = nlohmann::json::parse(t.payload, nullptr, false);
    if (!j.is_object()) throw Error("assertion: bad claims");
    Claims c;
    auto s = [&](const char* n) { return j.contains(n) && j[n].is_string() ? j[n].get<std::string>() : std::string(); };
    auto i = [&](const char* n) { return j.contains(n) && j[n].is_number_integer() ? j[n].get<int64_t>() : int64_t(0); };
    c.iss = s("iss"); c.aud = s("aud"); c.sub = s("sub"); c.jti = s("jti"); c.req = s("req"); c.client = s("client"); c.tenant = s("tenant");
    c.iat = i("iat"); c.exp = i("exp");
    if (j.contains("grants")) c.grants = policy::Grants::from_json(j["grants"]);
    int64_t now = now_fn();
    if (c.iss != issuer_) throw Error("assertion: wrong issuer");
    if (c.tenant != tenant_) throw Error("assertion: wrong tenant");
    if (c.aud != device_id) throw Error("assertion: wrong audience");
    if (c.req != request_id) throw Error("assertion: request id mismatch");
    if (c.exp - c.iat > kMaxLifetimeSec) throw Error("assertion: lifetime too long");
    // the gateway minted iat with its own clock just now: iat - now is how far this desktop's clock is behind
    const int64_t offset = c.iat - now;
    auto skew = [&](const char* what) {
        return ClockSkew(offset, std::string("assertion: ") + what + ": desktop clock " + std::to_string(offset < 0 ? -offset : offset) + " s " +
                                     (offset < 0 ? "ahead of" : "behind") + " the gateway (tolerance " + std::to_string(kFutureLeewaySec) + " s)");
    };
    if (c.iat > now + kFutureLeewaySec) throw skew("issued in the future");
    if (c.exp <= now - kExpiryLeewaySec)
        throw Expired(now - c.exp, offset, "assertion: expired " + std::to_string(now - c.exp) + " s ago (slack " + std::to_string(kExpiryLeewaySec) + " s)");
    if (c.jti.empty()) throw Error("assertion: missing jti");
    std::lock_guard<std::mutex> lk(mu_);
    for (auto it = seen_.begin(); it != seen_.end();) it = it->second < now - kSeenKeepSec ? seen_.erase(it) : std::next(it);
    if (!seen_.emplace(c.jti, c.exp).second) throw Error("assertion: replay");
    return c;
}

}  // namespace ffy::assertion
