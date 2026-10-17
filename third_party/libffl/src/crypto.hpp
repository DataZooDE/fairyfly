// OpenSSL 3 wrappers: EC P-256 / RSA keys, ES256 sign+verify, RS256 verify, JWK conversion.
#pragma once
#include <memory>
#include <nlohmann/json.hpp>
#include <string>
#include <string_view>

#include "util.hpp"

typedef struct evp_pkey_st EVP_PKEY;

namespace ffy {

class PKey {
public:
    PKey() = default;
    explicit PKey(EVP_PKEY* raw);  // takes ownership
    EVP_PKEY* get() const { return p_.get(); }
    explicit operator bool() const { return bool(p_); }

private:
    std::shared_ptr<EVP_PKEY> p_;
};

PKey ec_generate();
std::string ec_private_pem(const PKey& k);
PKey ec_private_from_pem(std::string_view pem);  // PKCS#8 or SEC1
PKey public_from_pem(std::string_view pem);      // SubjectPublicKeyInfo
PKey ec_public_from_xy(std::string_view x32, std::string_view y32);
PKey rsa_public_from_ne(std::string_view n, std::string_view e);
void ec_xy(const PKey& k, std::string& x32, std::string& y32);

std::string sha256(std::string_view in);

// Raw r||s (64 bytes) as used by JWS ES256.
std::string es256_sign(const PKey& priv, std::string_view msg);
bool es256_verify(const PKey& pub, std::string_view msg, std::string_view sig64);
bool rs256_verify(const PKey& pub, std::string_view msg, std::string_view sig);
std::string der_to_raw(std::string_view der);  // ASN.1 DER ECDSA signature -> r||s (throws Error)
std::string rsa_sign_for_tests(const PKey& priv, std::string_view msg);

struct Jwk {
    std::string kty, kid, alg, use, crv, x, y, n, e;
    nlohmann::json to_json() const;
    static Jwk from_json(const nlohmann::json& j);
    PKey ec() const;   // throws Error unless EC P-256
    PKey rsa() const;  // throws Error unless RSA
};
Jwk jwk_from_ec(const PKey& pub);                // kid = RFC 7638 thumbprint
std::string thumbprint(const PKey& ec_pub);      // base64url SHA-256

PKey rsa_generate_for_tests(int bits = 2048);
void rsa_ne(const PKey& k, std::string& n, std::string& e);

}  // namespace ffy
