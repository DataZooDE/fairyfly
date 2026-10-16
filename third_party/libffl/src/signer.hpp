// Signs call assertions. Production: Cloud KMS (the key never enters the container). Dev/tests: local key.
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "crypto.hpp"
#include "gcp.hpp"
#include "tenant.hpp"

namespace ffy {

class Signer {
public:
    virtual ~Signer() = default;
    virtual std::string sign(const std::string& msg) = 0;  // raw r||s (64 bytes)
    virtual std::string kid() = 0;                          // RFC 7638 thumbprint of the public key
    virtual std::vector<Jwk> public_keys() = 0;
};

// pem_path empty -> ephemeral key (devices must re-enrol after a restart).
std::unique_ptr<Signer> make_local_signer(const std::string& pem_path = {});
// key_version: projects/P/locations/L/keyRings/R/cryptoKeys/K/cryptoKeyVersions/N (EC_SIGN_P256_SHA256).
// base defaults to $FFY_KMS_BASE or https://cloudkms.googleapis.com. Fetches the public key eagerly (fail fast).
std::unique_ptr<Signer> make_kms_signer(std::shared_ptr<GcpToken> auth, std::string key_version, std::string base = {});

// The signing key of a tenant. Multi-tenant deployments use one key per tenant, so a desktop pinned to its tenant's
// key cannot be driven with an assertion minted for another tenant, even if the gateway mis-routes.
class SignerProvider {
public:
    virtual ~SignerProvider() = default;
    virtual std::shared_ptr<Signer> for_tenant(const TenantScope& t) = 0;  // throws ffy::Error if the tenant has no key

    static std::shared_ptr<SignerProvider> single(std::shared_ptr<Signer> s);  // one key for every tenant (single-tenant)
    static std::shared_ptr<SignerProvider> by_tenant(std::map<std::string, std::shared_ptr<Signer>> m);
    // Creates signers on first use and keeps them (e.g. KMS keys named after the tenant).
    static std::shared_ptr<SignerProvider> lazy(std::function<std::shared_ptr<Signer>(const TenantScope&)> make);
};

}  // namespace ffy
