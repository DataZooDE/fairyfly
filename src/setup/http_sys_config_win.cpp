// Native http.sys service configuration: URL ACL reservations and SSL certificate bindings
// (HttpSetServiceConfiguration / HttpQueryServiceConfiguration / HttpDeleteServiceConfiguration).
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <http.h>
#include <objbase.h>

#include <cstring>
#include <vector>

#include "include/setup/setup_hosts.h"
#include "win_util.h"

namespace fairyfly::setup {

namespace {

using namespace win;

/// HttpInitialize(HTTP_INITIALIZE_CONFIG) for the lifetime of one operation.
struct HttpConfigScope {
    HttpConfigScope() {
        HTTPAPI_VERSION version = HTTPAPI_VERSION_1;
        const ULONG r = HttpInitialize(version, HTTP_INITIALIZE_CONFIG, nullptr);
        if (r != NO_ERROR) throw_win(r, "HttpInitialize");
    }
    ~HttpConfigScope() { HttpTerminate(HTTP_INITIALIZE_CONFIG, nullptr); }
};

struct SockAddr {
    SOCKADDR_STORAGE storage{};
    int length = 0;
    PSOCKADDR get() { return reinterpret_cast<PSOCKADDR>(&storage); }
};

SockAddr make_sockaddr(const std::string& ipport) {
    std::string ip;
    int port = 0;
    if (!parse_ipport(ipport, &ip, &port)) throw HostError("INVALID_ARGUMENT", "malformed ip:port " + ipport);
    SockAddr out;
    auto* v4 = reinterpret_cast<SOCKADDR_IN*>(&out.storage);
    if (inet_pton(AF_INET, ip.c_str(), &v4->sin_addr) == 1) {
        v4->sin_family = AF_INET;
        v4->sin_port = htons(static_cast<u_short>(port));
        out.length = sizeof(SOCKADDR_IN);
        return out;
    }
    auto* v6 = reinterpret_cast<SOCKADDR_IN6*>(&out.storage);
    if (inet_pton(AF_INET6, ip.c_str(), &v6->sin6_addr) == 1) {
        v6->sin6_family = AF_INET6;
        v6->sin6_port = htons(static_cast<u_short>(port));
        out.length = sizeof(SOCKADDR_IN6);
        return out;
    }
    throw HostError("INVALID_ARGUMENT", "malformed address " + ipport);
}

std::string guid_to_string(const GUID& g) {
    wchar_t buffer[64] = {};
    StringFromGUID2(g, buffer, 64);
    return normalize_app_id(narrow(buffer));
}

GUID string_to_guid(const std::string& s) {
    GUID g{};
    const std::wstring w = widen(s);
    if (CLSIDFromString(w.c_str(), &g) != S_OK) throw HostError("INVALID_ARGUMENT", "malformed AppId " + s);
    return g;
}

class RealHttpSysConfig : public HttpSysConfig {
public:
    std::optional<std::string> query_urlacl(const std::string& prefix) override {
        HttpConfigScope scope;
        std::wstring wprefix = widen(prefix);
        HTTP_SERVICE_CONFIG_URLACL_QUERY query{};
        query.QueryDesc = HttpServiceConfigQueryExact;
        query.KeyDesc.pUrlPrefix = wprefix.data();
        ULONG need = 0;
        ULONG r = HttpQueryServiceConfiguration(nullptr, HttpServiceConfigUrlAclInfo, &query, sizeof(query), nullptr, 0, &need, nullptr);
        if (r == ERROR_FILE_NOT_FOUND) return std::nullopt;
        if (r != ERROR_INSUFFICIENT_BUFFER && r != NO_ERROR) throw_win(r, "query urlacl");
        std::vector<BYTE> buffer(need ? need : 1024);
        r = HttpQueryServiceConfiguration(nullptr, HttpServiceConfigUrlAclInfo, &query, sizeof(query), buffer.data(),
                                          static_cast<ULONG>(buffer.size()), &need, nullptr);
        if (r == ERROR_FILE_NOT_FOUND) return std::nullopt;
        if (r != NO_ERROR) throw_win(r, "query urlacl");
        const auto* set = reinterpret_cast<HTTP_SERVICE_CONFIG_URLACL_SET*>(buffer.data());
        return set->ParamDesc.pStringSecurityDescriptor ? narrow(set->ParamDesc.pStringSecurityDescriptor) : std::string();
    }

    void add_urlacl(const std::string& prefix, const std::string& sddl) override {
        HttpConfigScope scope;
        std::wstring wprefix = widen(prefix), wsddl = widen(sddl);
        HTTP_SERVICE_CONFIG_URLACL_SET set{};
        set.KeyDesc.pUrlPrefix = wprefix.data();
        set.ParamDesc.pStringSecurityDescriptor = wsddl.data();
        const ULONG r = HttpSetServiceConfiguration(nullptr, HttpServiceConfigUrlAclInfo, &set, sizeof(set), nullptr);
        if (r != NO_ERROR) throw_win(r, "reserve " + prefix);
    }

    bool remove_urlacl(const std::string& prefix) override {
        const auto existing = query_urlacl(prefix);
        if (!existing) return false;
        HttpConfigScope scope;
        std::wstring wprefix = widen(prefix), wsddl = widen(*existing);
        HTTP_SERVICE_CONFIG_URLACL_SET set{};
        set.KeyDesc.pUrlPrefix = wprefix.data();
        set.ParamDesc.pStringSecurityDescriptor = wsddl.data();
        const ULONG r = HttpDeleteServiceConfiguration(nullptr, HttpServiceConfigUrlAclInfo, &set, sizeof(set), nullptr);
        if (r == ERROR_FILE_NOT_FOUND) return false;
        if (r != NO_ERROR) throw_win(r, "delete reservation " + prefix);
        return true;
    }

    std::optional<SslBinding> query_sslcert(const std::string& ipport) override {
        HttpConfigScope scope;
        SockAddr addr = make_sockaddr(ipport);
        HTTP_SERVICE_CONFIG_SSL_QUERY query{};
        query.QueryDesc = HttpServiceConfigQueryExact;
        query.KeyDesc.pIpPort = addr.get();
        ULONG need = 0;
        ULONG r = HttpQueryServiceConfiguration(nullptr, HttpServiceConfigSSLCertInfo, &query, sizeof(query), nullptr, 0, &need, nullptr);
        if (r == ERROR_FILE_NOT_FOUND) return std::nullopt;
        if (r != ERROR_INSUFFICIENT_BUFFER && r != NO_ERROR) throw_win(r, "query sslcert " + ipport);
        std::vector<BYTE> buffer(need ? need : 1024);
        r = HttpQueryServiceConfiguration(nullptr, HttpServiceConfigSSLCertInfo, &query, sizeof(query), buffer.data(),
                                          static_cast<ULONG>(buffer.size()), &need, nullptr);
        if (r == ERROR_FILE_NOT_FOUND) return std::nullopt;
        if (r != NO_ERROR) throw_win(r, "query sslcert " + ipport);
        const auto* set = reinterpret_cast<HTTP_SERVICE_CONFIG_SSL_SET*>(buffer.data());
        SslBinding b;
        b.ipport = ipport;
        b.thumbprint = hex_upper(static_cast<const unsigned char*>(set->ParamDesc.pSslHash), set->ParamDesc.SslHashLength);
        b.app_id = guid_to_string(set->ParamDesc.AppId);
        b.store = set->ParamDesc.pSslCertStoreName ? narrow(set->ParamDesc.pSslCertStoreName) : std::string();
        return b;
    }

    void set_sslcert(const std::string& ipport, const std::string& thumbprint) override {
        const auto hash = hex_to_bytes(thumbprint);
        if (hash.size() != 20) throw HostError("INVALID_ARGUMENT", "thumbprint must be 20 bytes");
        if (query_sslcert(ipport)) remove_sslcert(ipport);   // create-or-replace
        HttpConfigScope scope;
        SockAddr addr = make_sockaddr(ipport);
        std::wstring store = widen(kCertStore);
        std::vector<unsigned char> hash_copy = hash;
        HTTP_SERVICE_CONFIG_SSL_SET set{};
        set.KeyDesc.pIpPort = addr.get();
        set.ParamDesc.SslHashLength = static_cast<ULONG>(hash_copy.size());
        set.ParamDesc.pSslHash = hash_copy.data();
        set.ParamDesc.AppId = string_to_guid(kAppId);
        set.ParamDesc.pSslCertStoreName = store.data();
        set.ParamDesc.DefaultFlags = 0;   // no client certificate negotiation, revocation checks apply to client certs only
        const ULONG r = HttpSetServiceConfiguration(nullptr, HttpServiceConfigSSLCertInfo, &set, sizeof(set), nullptr);
        if (r != NO_ERROR) throw_win(r, "bind certificate to " + ipport);
    }

    bool remove_sslcert(const std::string& ipport) override {
        if (!query_sslcert(ipport)) return false;
        HttpConfigScope scope;
        SockAddr addr = make_sockaddr(ipport);
        HTTP_SERVICE_CONFIG_SSL_SET set{};
        set.KeyDesc.pIpPort = addr.get();
        const ULONG r = HttpDeleteServiceConfiguration(nullptr, HttpServiceConfigSSLCertInfo, &set, sizeof(set), nullptr);
        if (r == ERROR_FILE_NOT_FOUND) return false;
        if (r != NO_ERROR) throw_win(r, "delete binding " + ipport);
        return true;
    }
};

} // namespace

std::unique_ptr<HttpSysConfig> make_real_http_sys_config() { return std::make_unique<RealHttpSysConfig>(); }

} // namespace fairyfly::setup
