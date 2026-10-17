#include "crypto.hpp"

#include <openssl/bn.h>
#include <openssl/core_names.h>
#include <openssl/ecdsa.h>
#include <openssl/evp.h>
#include <openssl/param_build.h>
#include <openssl/pem.h>
#include <openssl/x509.h>

#include <memory>

namespace ffy {

namespace {
template <class T, void (*F)(T*)>
struct Del {
    void operator()(T* p) const { if (p) F(p); }
};
struct BioDel { void operator()(BIO* b) const { BIO_free(b); } };
using BioP = std::unique_ptr<BIO, BioDel>;
using BnP = std::unique_ptr<BIGNUM, Del<BIGNUM, BN_free>>;
using CtxP = std::unique_ptr<EVP_PKEY_CTX, Del<EVP_PKEY_CTX, EVP_PKEY_CTX_free>>;
using MdCtxP = std::unique_ptr<EVP_MD_CTX, Del<EVP_MD_CTX, EVP_MD_CTX_free>>;
using BldP = std::unique_ptr<OSSL_PARAM_BLD, Del<OSSL_PARAM_BLD, OSSL_PARAM_BLD_free>>;
using ParamsP = std::unique_ptr<OSSL_PARAM, Del<OSSL_PARAM, OSSL_PARAM_free>>;
using SigP = std::unique_ptr<ECDSA_SIG, Del<ECDSA_SIG, ECDSA_SIG_free>>;

std::string pad32(std::string s) {
    if (s.size() > 32) throw Error("integer too large for P-256");
    return std::string(32 - s.size(), '\0') + s;
}
std::string bn_bytes(const BIGNUM* b) {
    std::string s(size_t(BN_num_bytes(b)), '\0');
    BN_bn2bin(b, reinterpret_cast<unsigned char*>(s.data()));
    return s;
}
}  // namespace

PKey::PKey(EVP_PKEY* raw) : p_(raw, EVP_PKEY_free) {}

PKey ec_generate() {
    CtxP c(EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr));
    EVP_PKEY* k = nullptr;
    if (!c || EVP_PKEY_keygen_init(c.get()) <= 0 || EVP_PKEY_CTX_set_ec_paramgen_curve_nid(c.get(), NID_X9_62_prime256v1) <= 0 ||
        EVP_PKEY_keygen(c.get(), &k) <= 0)
        throw Error("EC keygen failed");
    return PKey(k);
}

std::string ec_private_pem(const PKey& k) {
    BioP b(BIO_new(BIO_s_mem()));
    if (PEM_write_bio_PrivateKey(b.get(), k.get(), nullptr, nullptr, 0, nullptr, nullptr) != 1) throw Error("PEM write failed");
    char* d;
    long n = BIO_get_mem_data(b.get(), &d);
    return std::string(d, size_t(n));
}

PKey ec_private_from_pem(std::string_view pem) {
    BioP b(BIO_new_mem_buf(pem.data(), int(pem.size())));
    EVP_PKEY* k = PEM_read_bio_PrivateKey(b.get(), nullptr, nullptr, nullptr);
    if (!k) throw Error("cannot parse private key PEM");
    return PKey(k);
}

PKey public_from_pem(std::string_view pem) {
    BioP b(BIO_new_mem_buf(pem.data(), int(pem.size())));
    EVP_PKEY* k = PEM_read_bio_PUBKEY(b.get(), nullptr, nullptr, nullptr);
    if (!k) throw Error("cannot parse public key PEM");
    return PKey(k);
}

PKey ec_public_from_xy(std::string_view x, std::string_view y) {
    std::string pt = "\x04" + pad32(std::string(x)) + pad32(std::string(y));
    BldP bld(OSSL_PARAM_BLD_new());
    OSSL_PARAM_BLD_push_utf8_string(bld.get(), OSSL_PKEY_PARAM_GROUP_NAME, "prime256v1", 0);
    OSSL_PARAM_BLD_push_octet_string(bld.get(), OSSL_PKEY_PARAM_PUB_KEY, pt.data(), pt.size());
    ParamsP params(OSSL_PARAM_BLD_to_param(bld.get()));
    CtxP ctx(EVP_PKEY_CTX_new_from_name(nullptr, "EC", nullptr));
    EVP_PKEY* k = nullptr;
    if (!ctx || EVP_PKEY_fromdata_init(ctx.get()) <= 0 || EVP_PKEY_fromdata(ctx.get(), &k, EVP_PKEY_PUBLIC_KEY, params.get()) <= 0)
        throw Error("point is not a valid P-256 public key");
    return PKey(k);
}

PKey rsa_public_from_ne(std::string_view n, std::string_view e) {
    BnP bn(BN_bin2bn(reinterpret_cast<const unsigned char*>(n.data()), int(n.size()), nullptr));
    BnP be(BN_bin2bn(reinterpret_cast<const unsigned char*>(e.data()), int(e.size()), nullptr));
    BldP bld(OSSL_PARAM_BLD_new());
    OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_RSA_N, bn.get());
    OSSL_PARAM_BLD_push_BN(bld.get(), OSSL_PKEY_PARAM_RSA_E, be.get());
    ParamsP params(OSSL_PARAM_BLD_to_param(bld.get()));
    CtxP ctx(EVP_PKEY_CTX_new_from_name(nullptr, "RSA", nullptr));
    EVP_PKEY* k = nullptr;
    if (!ctx || EVP_PKEY_fromdata_init(ctx.get()) <= 0 || EVP_PKEY_fromdata(ctx.get(), &k, EVP_PKEY_PUBLIC_KEY, params.get()) <= 0)
        throw Error("invalid RSA public key");
    return PKey(k);
}

void ec_xy(const PKey& k, std::string& x, std::string& y) {
    unsigned char buf[80];
    size_t len = 0;
    if (EVP_PKEY_get_octet_string_param(k.get(), OSSL_PKEY_PARAM_PUB_KEY, buf, sizeof buf, &len) != 1 || len != 65 || buf[0] != 4)
        throw Error("not an uncompressed P-256 key");
    x.assign(reinterpret_cast<char*>(buf + 1), 32);
    y.assign(reinterpret_cast<char*>(buf + 33), 32);
}

void rsa_ne(const PKey& k, std::string& n, std::string& e) {
    BIGNUM *bn = nullptr, *be = nullptr;
    if (EVP_PKEY_get_bn_param(k.get(), OSSL_PKEY_PARAM_RSA_N, &bn) != 1 || EVP_PKEY_get_bn_param(k.get(), OSSL_PKEY_PARAM_RSA_E, &be) != 1)
        throw Error("not an RSA key");
    BnP a(bn), b(be);
    n = bn_bytes(bn);
    e = bn_bytes(be);
}

std::string sha256(std::string_view in) {
    unsigned char md[32];
    unsigned int len = 0;
    EVP_Digest(in.data(), in.size(), md, &len, EVP_sha256(), nullptr);
    return std::string(reinterpret_cast<char*>(md), len);
}

std::string der_to_raw(std::string_view der) {
    const unsigned char* p = reinterpret_cast<const unsigned char*>(der.data());
    SigP sig(d2i_ECDSA_SIG(nullptr, &p, long(der.size())));
    if (!sig || p != reinterpret_cast<const unsigned char*>(der.data()) + der.size()) throw Error("malformed DER signature");
    const BIGNUM *r, *s;
    ECDSA_SIG_get0(sig.get(), &r, &s);
    if (BN_is_zero(r) || BN_is_zero(s) || BN_is_negative(r) || BN_is_negative(s)) throw Error("malformed DER signature");
    return pad32(bn_bytes(r)) + pad32(bn_bytes(s));
}

std::string es256_sign(const PKey& priv, std::string_view msg) {
    MdCtxP c(EVP_MD_CTX_new());
    size_t len = 0;
    if (EVP_DigestSignInit(c.get(), nullptr, EVP_sha256(), nullptr, priv.get()) != 1 ||
        EVP_DigestSign(c.get(), nullptr, &len, reinterpret_cast<const unsigned char*>(msg.data()), msg.size()) != 1)
        throw Error("sign failed");
    std::string der(len, '\0');
    if (EVP_DigestSign(c.get(), reinterpret_cast<unsigned char*>(der.data()), &len, reinterpret_cast<const unsigned char*>(msg.data()), msg.size()) != 1)
        throw Error("sign failed");
    der.resize(len);
    return der_to_raw(der);
}

bool es256_verify(const PKey& pub, std::string_view msg, std::string_view sig64) {
    if (!pub || sig64.size() != 64) return false;
    BnP r(BN_bin2bn(reinterpret_cast<const unsigned char*>(sig64.data()), 32, nullptr));
    BnP s(BN_bin2bn(reinterpret_cast<const unsigned char*>(sig64.data()) + 32, 32, nullptr));
    SigP sig(ECDSA_SIG_new());
    if (!r || !s || !sig || ECDSA_SIG_set0(sig.get(), r.get(), s.get()) != 1) return false;
    r.release();
    s.release();
    unsigned char* der = nullptr;
    int dl = i2d_ECDSA_SIG(sig.get(), &der);
    if (dl <= 0) return false;
    MdCtxP c(EVP_MD_CTX_new());
    bool ok = EVP_DigestVerifyInit(c.get(), nullptr, EVP_sha256(), nullptr, pub.get()) == 1 &&
              EVP_DigestVerify(c.get(), der, size_t(dl), reinterpret_cast<const unsigned char*>(msg.data()), msg.size()) == 1;
    OPENSSL_free(der);
    return ok;
}

bool rs256_verify(const PKey& pub, std::string_view msg, std::string_view sig) {
    if (!pub) return false;
    MdCtxP c(EVP_MD_CTX_new());
    return EVP_DigestVerifyInit(c.get(), nullptr, EVP_sha256(), nullptr, pub.get()) == 1 &&
           EVP_DigestVerify(c.get(), reinterpret_cast<const unsigned char*>(sig.data()), sig.size(), reinterpret_cast<const unsigned char*>(msg.data()),
                            msg.size()) == 1;
}

std::string rsa_sign_for_tests(const PKey& priv, std::string_view msg) {
    MdCtxP c(EVP_MD_CTX_new());
    size_t len = 0;
    if (EVP_DigestSignInit(c.get(), nullptr, EVP_sha256(), nullptr, priv.get()) != 1 ||
        EVP_DigestSign(c.get(), nullptr, &len, reinterpret_cast<const unsigned char*>(msg.data()), msg.size()) != 1)
        throw Error("rsa sign failed");
    std::string sig(len, '\0');
    if (EVP_DigestSign(c.get(), reinterpret_cast<unsigned char*>(sig.data()), &len, reinterpret_cast<const unsigned char*>(msg.data()), msg.size()) != 1)
        throw Error("rsa sign failed");
    sig.resize(len);
    return sig;
}

PKey rsa_generate_for_tests(int bits) {
    CtxP c(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr));
    EVP_PKEY* k = nullptr;
    if (!c || EVP_PKEY_keygen_init(c.get()) <= 0 || EVP_PKEY_CTX_set_rsa_keygen_bits(c.get(), bits) <= 0 || EVP_PKEY_keygen(c.get(), &k) <= 0)
        throw Error("RSA keygen failed");
    return PKey(k);
}

std::string thumbprint(const PKey& pub) {
    std::string x, y;
    ec_xy(pub, x, y);
    return b64url_encode(sha256(R"({"crv":"P-256","kty":"EC","x":")" + b64url_encode(x) + R"(","y":")" + b64url_encode(y) + R"("})"));
}

Jwk jwk_from_ec(const PKey& pub) {
    std::string x, y;
    ec_xy(pub, x, y);
    Jwk j;
    j.kty = "EC";
    j.crv = "P-256";
    j.alg = "ES256";
    j.use = "sig";
    j.x = b64url_encode(x);
    j.y = b64url_encode(y);
    j.kid = thumbprint(pub);
    return j;
}

nlohmann::json Jwk::to_json() const {
    nlohmann::json j;
    auto put = [&](const char* k, const std::string& v) {
        if (!v.empty()) j[k] = v;
    };
    put("kty", kty); put("kid", kid); put("alg", alg); put("use", use); put("crv", crv); put("x", x); put("y", y); put("n", n); put("e", e);
    return j;
}

Jwk Jwk::from_json(const nlohmann::json& j) {
    Jwk k;
    auto get = [&](const char* name) { return j.contains(name) && j[name].is_string() ? j[name].get<std::string>() : std::string(); };
    k.kty = get("kty"); k.kid = get("kid"); k.alg = get("alg"); k.use = get("use"); k.crv = get("crv");
    k.x = get("x"); k.y = get("y"); k.n = get("n"); k.e = get("e");
    return k;
}

PKey Jwk::ec() const {
    if (kty != "EC" || crv != "P-256") throw Error("jwk: not an EC P-256 key");
    return ec_public_from_xy(b64url_decode(x), b64url_decode(y));
}

PKey Jwk::rsa() const {
    if (kty != "RSA") throw Error("jwk: not an RSA key");
    return rsa_public_from_ne(b64url_decode(n), b64url_decode(e));
}

}  // namespace ffy
