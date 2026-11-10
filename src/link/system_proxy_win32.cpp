// The Windows side of the proxy settings (include/link/system_proxy.h): WinHTTP for the user's settings, WPAD and PAC
// scripts, SSPI for Negotiate/NTLM with the logged-on user's credentials, the Credential Manager for an explicitly
// stored Basic credential. Nothing here logs a credential or a token.
#include "include/link/system_proxy.h"

#include <windows.h>
#include <winhttp.h>
#define SECURITY_WIN32
#include <security.h>

#include <spdlog/spdlog.h>

#include <chrono>
#include <map>
#include <memory>
#include <mutex>

#include "include/credential_store.h"
#include "include/text/utf8.h"
#include "util.hpp"  // libffl

namespace fairyfly::link::sysproxy {

namespace {

std::string take(LPWSTR& p) {
    std::string s = p ? text::wide_to_utf8_lossy(p) : std::string();
    if (p) GlobalFree(p);
    p = nullptr;
    return s;
}

std::string winhttp_error(DWORD e) {
    switch (e) {
    case ERROR_WINHTTP_AUTODETECTION_FAILED: return "no proxy configuration found by automatic detection";
    case ERROR_WINHTTP_UNABLE_TO_DOWNLOAD_SCRIPT: return "the proxy script (PAC) could not be downloaded";
    case ERROR_WINHTTP_BAD_AUTO_PROXY_SCRIPT: return "the proxy script (PAC) has an error";
    case ERROR_WINHTTP_LOGIN_FAILURE: return "the proxy script's server refused the Windows login";
    case ERROR_WINHTTP_TIMEOUT: return "timeout";
    default: return "WinHTTP error " + std::to_string(e);
    }
}

// The administrator's opt-in for the Windows login to WPAD-discovered proxies (HKLM policy, off by default).
bool discovered_login_policy() {
    DWORD v = 0, size = sizeof v;
    return RegGetValueW(HKEY_LOCAL_MACHINE, kPolicyKey, kDiscoveredLoginPolicy, RRF_RT_REG_DWORD, nullptr, &v, &size) == ERROR_SUCCESS && v == 1;
}

// The PAC URL group policy sets ("Use automatic configuration script"), machine before user; "" when none.
std::string policy_pac_url() {
    const wchar_t* key = L"Software\\Policies\\Microsoft\\Windows\\CurrentVersion\\Internet Settings";
    for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
        wchar_t buf[2048];
        DWORD size = sizeof buf;
        if (RegGetValueW(root, key, L"AutoConfigURL", RRF_RT_REG_SZ, nullptr, buf, &size) == ERROR_SUCCESS)
            return text::wide_to_utf8_lossy(buf);
    }
    return {};
}

ProxySettings user_settings() {
    WINHTTP_CURRENT_USER_IE_PROXY_CONFIG c{};
    ProxySettings s;
    if (!WinHttpGetIEProxyConfigForCurrentUser(&c)) {
        spdlog::debug("proxy: no proxy settings for this user ({})", winhttp_error(GetLastError()));
        return s;
    }
    s.auto_detect = c.fAutoDetect != FALSE;
    s.auto_config_url = take(c.lpszAutoConfigUrl);
    s.pac_by_policy = !s.auto_config_url.empty() && policy_pac_url() == s.auto_config_url;
    s.proxy = take(c.lpszProxy);
    s.bypass = take(c.lpszProxyBypass);
    return s;
}

std::optional<ProxySettings> machine_settings() {
    WINHTTP_PROXY_INFO i{};
    if (!WinHttpGetDefaultProxyConfiguration(&i)) return std::nullopt;
    ProxySettings s;
    s.proxy = take(i.lpszProxy);
    s.bypass = take(i.lpszProxyBypass);
    if (i.dwAccessType != WINHTTP_ACCESS_TYPE_NAMED_PROXY) return std::nullopt;
    return s;
}

std::string describe(const ffy::proxy::Routes& routes) {
    if (routes.empty()) return "DIRECT";
    std::string s;
    for (auto& r : routes) s += (s.empty() ? "" : ", ") + (r.direct ? std::string("DIRECT") : r.proxy.str());
    return s;
}

// The routes per scheme://host:port, cached for 5 minutes: automatic detection can take seconds, and the link asks again
// on every reconnect. One WinHTTP session keeps WinHTTP's own script cache.
class Resolver {
public:
    ~Resolver() {
        if (session_) WinHttpCloseHandle(session_);
    }
    ffy::proxy::Routes routes(const std::string& url) {
        const ffy::Url u = ffy::parse_url(url);
        const std::string key = u.scheme + "://" + u.host + ":" + u.port;
        std::lock_guard<std::mutex> l(mu_);
        const auto now = std::chrono::steady_clock::now();
        if (auto i = cache_.find(key); i != cache_.end() && now - i->second.at < std::chrono::minutes(5)) return i->second.routes;
        const ProxySettings user = user_settings();
        const Resolution r = resolve(user, machine_settings(), url, [&](AutoSource src) { return autoproxy(user, url, src); });
        const std::string line = describe(r.routes) + " (" + r.how + ")";
        if (logged_[key] != line) {  // once per change, not on every reconnect
            spdlog::info("proxy: {} -> {}", u.host, line);
            logged_[key] = line;
        }
        cache_[key] = Cached{now, r.routes};
        return r.routes;
    }

private:
    // One source per call (the script, or detection), so that each route knows where it came from (Origin).
    AutoProxyResult autoproxy(const ProxySettings& user, const std::string& url, AutoSource src) {
        AutoProxyResult r;
        if (!session_) {
            session_ = WinHttpOpen(L"fairyfly", WINHTTP_ACCESS_TYPE_NO_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
            if (!session_) {
                r.error = winhttp_error(GetLastError());
                return r;
            }
            WinHttpSetTimeouts(session_, 5000, 5000, 10000, 10000);  // the PAC download
        }
        WINHTTP_AUTOPROXY_OPTIONS o{};
        if (src == AutoSource::wpad) {
            o.dwFlags |= WINHTTP_AUTOPROXY_AUTO_DETECT;
            o.dwAutoDetectFlags = WINHTTP_AUTO_DETECT_TYPE_DHCP | WINHTTP_AUTO_DETECT_TYPE_DNS_A;
        }
        std::wstring pac, wurl;
        try {  // strict: these decide where the traffic goes
            if (src == AutoSource::pac) pac = text::utf8_to_wide(user.auto_config_url);
            wurl = text::utf8_to_wide(url);
        } catch (const std::invalid_argument&) {
            r.error = "the proxy script address is not valid UTF-8";
            return r;
        }
        if (!pac.empty()) {
            o.dwFlags |= WINHTTP_AUTOPROXY_CONFIG_URL;
            o.lpszAutoConfigUrl = pac.c_str();
        }
        WINHTTP_PROXY_INFO info{};
        BOOL ok = WinHttpGetProxyForUrl(session_, wurl.c_str(), &o, &info);
        // the script's server wants the Windows login: only a trusted configured script (or with the administrator's WPAD
        // policy) gets it; a rogue "wpad" host asking for NTLM gets nothing
        if (!ok && GetLastError() == ERROR_WINHTTP_LOGIN_FAILURE &&
            script_logon_allowed(src, pac_trusted(user.auto_config_url, user.pac_by_policy), discovered_login_policy())) {
            o.fAutoLogonIfChallenged = TRUE;
            ok = WinHttpGetProxyForUrl(session_, wurl.c_str(), &o, &info);
        }
        if (!ok) {
            r.error = winhttp_error(GetLastError());
            return r;
        }
        r.ok = true;
        r.proxy = take(info.lpszProxy);
        r.bypass = take(info.lpszProxyBypass);
        r.direct = info.dwAccessType == WINHTTP_ACCESS_TYPE_NO_PROXY || r.proxy.empty();
        return r;
    }

    struct Cached {
        std::chrono::steady_clock::time_point at;
        ffy::proxy::Routes routes;
    };
    std::mutex mu_;
    HINTERNET session_ = nullptr;
    std::map<std::string, Cached> cache_;
    std::map<std::string, std::string> logged_;
};

// Negotiate (Kerberos, else NTLM inside it) or NTLM with the logged-on user's credentials. One instance per proxy
// connection: the security context belongs to that connection.
class SspiAuthenticator final : public ffy::proxy::Authenticator {
public:
    explicit SspiAuthenticator(const std::string& proxy_host) : spn_(proxy_spn(proxy_host)) {}
    ~SspiAuthenticator() override {
        if (have_ctx_) DeleteSecurityContext(&ctx_);
        if (have_cred_) FreeCredentialsHandle(&cred_);
    }
    std::optional<std::string> respond(const std::vector<std::string>& challenges) override {
        auto c = pick_sspi_challenge(challenges, scheme_);
        if (!c) return std::nullopt;
        if (scheme_.empty()) {
            if (!c->token.empty()) return std::nullopt;  // a continuation of a handshake that is not ours
            std::wstring package = c->scheme == "Negotiate" ? L"Negotiate" : L"NTLM";
            TimeStamp expiry{};
            SECURITY_STATUS st = AcquireCredentialsHandleW(nullptr, package.data(), SECPKG_CRED_OUTBOUND, nullptr, nullptr, nullptr,
                                                           nullptr, &cred_, &expiry);
            if (st != SEC_E_OK) {
                spdlog::warn("proxy: {} with the Windows login is not available (0x{:08x})", c->scheme, static_cast<unsigned long>(st));
                return std::nullopt;
            }
            have_cred_ = true;
            scheme_ = c->scheme;
            return step(nullptr);
        }
        if (c->token.empty() || done_) return std::nullopt;  // the proxy did not accept the handshake
        std::string in;
        try {
            in = ffy::b64std_decode(c->token);
        } catch (const std::exception&) {
            return std::nullopt;
        }
        return step(&in);
    }
    std::string scheme() const override { return scheme_; }

private:
    std::optional<std::string> step(const std::string* in) {
        SecBuffer in_buf{in ? static_cast<unsigned long>(in->size()) : 0ul, SECBUFFER_TOKEN, in ? const_cast<char*>(in->data()) : nullptr};
        SecBufferDesc in_desc{SECBUFFER_VERSION, 1, &in_buf};
        SecBuffer out_buf{0, SECBUFFER_TOKEN, nullptr};
        SecBufferDesc out_desc{SECBUFFER_VERSION, 1, &out_buf};
        unsigned long attrs = 0;
        TimeStamp expiry{};
        SECURITY_STATUS st = InitializeSecurityContextW(&cred_, have_ctx_ ? &ctx_ : nullptr, spn_.data(), ISC_REQ_ALLOCATE_MEMORY, 0,
                                                        SECURITY_NATIVE_DREP, in ? &in_desc : nullptr, 0, &ctx_, &out_desc, &attrs, &expiry);
        struct Free {
            SecBuffer& b;
            ~Free() {
                if (b.pvBuffer) FreeContextBuffer(b.pvBuffer);
            }
        } free_out{out_buf};
        if (st == SEC_I_COMPLETE_NEEDED || st == SEC_I_COMPLETE_AND_CONTINUE) {
            CompleteAuthToken(&ctx_, &out_desc);
            st = st == SEC_I_COMPLETE_NEEDED ? SEC_E_OK : SEC_I_CONTINUE_NEEDED;
        }
        if (st != SEC_E_OK && st != SEC_I_CONTINUE_NEEDED) {
            spdlog::warn("proxy: {} with the Windows login failed (0x{:08x})", scheme_, static_cast<unsigned long>(st));
            return std::nullopt;
        }
        have_ctx_ = true;
        done_ = st == SEC_E_OK;
        if (!out_buf.pvBuffer || out_buf.cbBuffer == 0) return std::nullopt;
        return scheme_ + " " + ffy::b64std_encode(std::string(static_cast<const char*>(out_buf.pvBuffer), out_buf.cbBuffer));
    }

    std::wstring spn_;
    std::string scheme_;
    CredHandle cred_{};
    CtxtHandle ctx_{};
    bool have_cred_ = false, have_ctx_ = false, done_ = false;
};

// The stored credential "fairyfly-proxy:<host>" (generic, Credential Manager), read only when a proxy asks for Basic.
std::optional<StoredLogin> read_stored_login(const std::string& proxy_host) {
    std::string host = proxy_host;
    if (host.size() > 1 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    try {
        cred::WindowsCredentialStore store("fairyfly-proxy:");
        auto c = store.read(host);
        if (!c) return std::nullopt;
        return StoredLogin{c->meta.username, c->password.utf8()};
    } catch (const std::exception& e) {
        spdlog::warn("proxy: the stored credential fairyfly-proxy:{} cannot be read: {}", host, e.what());
        return std::nullopt;
    }
}

}  // namespace

std::unique_ptr<ffy::proxy::Authenticator> make_sspi_authenticator(const std::string& proxy_host) {
    return std::make_unique<SspiAuthenticator>(proxy_host);
}

void install_system_proxy() {
    static std::once_flag once;
    std::call_once(once, [] {
        auto resolver = std::make_shared<Resolver>();
        ffy::proxy::Config c;
        c.source = "Windows proxy settings";
        c.resolve = [resolver](const std::string& url) { return resolver->routes(url); };
        c.authenticators = {
            [](const ffy::proxy::Endpoint& e) {
                return login_authenticator(e, discovered_login_policy(), [&e] { return make_sspi_authenticator(e.host); });
            },
            [](const ffy::proxy::Endpoint& e) { return stored_basic(e, [host = e.host] { return read_stored_login(host); }); }};
        ffy::proxy::configure(std::move(c));
    });
}

}  // namespace fairyfly::link::sysproxy
