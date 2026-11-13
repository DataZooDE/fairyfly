#pragma once

// The Windows proxy settings for the cloud link and every other HTTPS request fairyfly makes through libffl
// (pairing, the link's WebSocket, update checks). libffl tunnels through the proxy (CONNECT) and asks the
// authenticators installed here. The Windows settings are the last of the sources include/link/proxy_config.h ranks
// (fairyfly policy, your fairyfly setting, HTTPS_PROXY / NO_PROXY, browser policies, then these).
//
// Where the Windows routes come from, in the order Windows itself uses:
//   1. automatic configuration of the current user (WinHttpGetIEProxyConfigForCurrentUser): a setup script (PAC URL),
//      then "Automatically detect settings" (WPAD), evaluated by WinHttpGetProxyForUrl for the URL;
//   2. if that is off or fails: the user's manual proxy server with its bypass list ("Don't use the proxy server for
//      local addresses" = <local>);
//   3. else WinHTTP's machine setting (netsh winhttp set proxy);
//   4. else DIRECT.
// Proxy authentication, by where the proxy came from (ffy::proxy::Origin):
//   - Negotiate (Kerberos) or NTLM with the logged-on user's Windows credentials (SSPI) for a proxy named explicitly
//     (manual settings, netsh/GPO machine setting, HTTPS_PROXY) or by the configured PAC script; NOT for a proxy found
//     by automatic detection (WPAD: anyone on the local network can answer it and collect NTLM hashes), unless the
//     administrator allows it by policy (kDiscoveredLoginPolicy);
//   - Basic only to a proxy named explicitly, and only when a credential is stored for it (Credential Manager, generic
//     target "fairyfly-proxy:<host>", e.g. `cmdkey /generic:fairyfly-proxy:proxy.corp.example /user:CORP\alice /pass`),
//     read only when the proxy asks for Basic.
//
// The parsing and decisions are portable (unit-tested); system_proxy_win32.cpp calls WinHTTP and SSPI.

#include "proxy.hpp"  // libffl

#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace fairyfly::link::sysproxy {

/// What Windows reports: the current user's settings (WinHttpGetIEProxyConfigForCurrentUser), or WinHTTP's machine
/// setting (WinHttpGetDefaultProxyConfiguration, then only proxy/bypass).
struct ProxySettings {
    bool auto_detect = false;     ///< WPAD
    std::string auto_config_url;  ///< PAC script URL
    bool pac_by_policy = false;   ///< the PAC URL is set by group policy (Software\Policies\...\Internet Settings)
    std::string proxy;            ///< "proxy:8080" or "http=a:80;https=b:8443" (also from a PAC result)
    std::string bypass;           ///< "<local>;*.corp.example;10.*"
};

/// The result of WinHttpGetProxyForUrl for one URL (WPAD/PAC).
struct AutoProxyResult {
    bool ok = false;      ///< false: detection or the script failed (error says why)
    bool direct = false;  ///< the script said DIRECT
    std::string proxy;    ///< proxy list, as in ProxySettings::proxy
    std::string bypass;
    std::string error;
};

/// A WinHTTP/IE proxy list for an https:// URL: "https=" entries, else entries without a scheme; "DIRECT" entries
/// stay in their place; socks= and other schemes' entries are ignored. Separators: ';' and whitespace.
ffy::proxy::Routes parse_proxy_list(const std::string& list);

/// The IE/WinHTTP bypass list: ';' (also ',' and whitespace) separated patterns with '*' wildcards, matched without
/// case against the host (and "host:port"); a leading "scheme://" is ignored, ".suffix" means "*.suffix", and
/// "<local>" matches every host name without a dot.
bool bypassed(const std::string& host, int port, const std::string& bypass_list);

struct Resolution {
    ffy::proxy::Routes routes;  ///< empty = DIRECT
    std::string how;            ///< for the log: "PAC http://wpad/proxy.pac", "manual proxy", "bypass list", ...
};

/// Which automatic configuration to evaluate: the configured script, or detection (WPAD).
enum class AutoSource { pac, wpad };

/// The routes for an https:// URL from the settings, in the order at the top of this file, each proxy marked with its
/// origin (configured, pac, discovered). `autoproxy` is called only for what the user's settings turn on: the PAC
/// script first (when set), detection only when there is no script or it failed.
Resolution resolve(const ProxySettings& user, const std::optional<ProxySettings>& machine, const std::string& url,
                   const std::function<AutoProxyResult(AutoSource)>& autoproxy);

/// Administrator policy that lets the Windows login go to proxies found by WPAD too:
/// HKLM\SOFTWARE\Policies\DataZoo\fairyfly, DWORD AllowLoginToDiscoveredProxies = 1. Off by default.
inline constexpr const wchar_t* kPolicyKey = L"SOFTWARE\\Policies\\DataZoo\\fairyfly";
inline constexpr const wchar_t* kDiscoveredLoginPolicy = L"AllowLoginToDiscoveredProxies";

/// Whether a configured PAC script is trusted with credentials: fetched over https://, or set by group policy with a
/// dotted host name (not a single-label "wpad"/"proxy" that anyone on the local network can claim). An untrusted script
/// is treated like WPAD: its proxies count as discovered.
bool pac_trusted(const std::string& pac_url, bool by_policy);

/// Whether WinHTTP may answer a 401 with the Windows login while DOWNLOADING a proxy script (fAutoLogonIfChallenged):
/// only for a trusted configured script, or with the administrator's WPAD policy on. Otherwise a rogue "wpad" host
/// could ask for NTLM before any proxy is named (WinHTTP treats single-label hosts as intranet).
bool script_logon_allowed(AutoSource src, bool pac_is_trusted, bool discovered_login_policy);

/// Whether this proxy may be offered the Windows login (Negotiate/NTLM).
bool windows_login_allowed(ffy::proxy::Origin origin, bool discovered_login_policy);

/// The Negotiate/NTLM authenticator for one proxy connection: `sspi()` (the real SSPI one) when the proxy may get the
/// Windows login, otherwise one that sends nothing, logs the refusal once and says why in the error.
std::unique_ptr<ffy::proxy::Authenticator> login_authenticator(const ffy::proxy::Endpoint& proxy, bool discovered_login_policy,
                                                                const std::function<std::unique_ptr<ffy::proxy::Authenticator>()>& sspi);

/// A stored credential: user name and password (the caller's buffers are wiped after use).
struct StoredLogin {
    std::string user, password;
};
/// Basic from a stored credential: `read` is called only when the proxy offers Basic, and only for a proxy named
/// explicitly (Origin::configured); everything is wiped after the header is built.
std::unique_ptr<ffy::proxy::Authenticator> stored_basic(const ffy::proxy::Endpoint& proxy,
                                                         std::function<std::optional<StoredLogin>()> read);

/// For an SSPI authenticator: the challenge to answer among the proxy's Proxy-Authenticate values. With no scheme
/// chosen yet, Negotiate before NTLM; afterwards only the chosen scheme (its token, empty when the proxy restarts or
/// rejects the handshake). nullopt: none of the two is offered.
struct SspiChallenge {
    std::string scheme;  ///< "Negotiate" or "NTLM"
    std::string token;   ///< base64, may be empty
};
std::optional<SspiChallenge> pick_sspi_challenge(const std::vector<std::string>& challenges, const std::string& chosen);

/// The SPN for Negotiate against a proxy: "HTTP/<host>" (brackets of an IPv6 literal removed).
std::wstring proxy_spn(const std::string& proxy_host);

/// Installs the resolver of include/link/proxy_config.h (all sources, Windows last) and the authenticators into libffl
/// (once, at start; proxycfg::process_resolver() is it afterwards). No-op outside Windows.
void install_system_proxy();

/// A Negotiate/NTLM authenticator with the logged-on user's credentials (SSPI). Windows only; nullptr elsewhere.
std::unique_ptr<ffy::proxy::Authenticator> make_sspi_authenticator(const std::string& proxy_host);

}  // namespace fairyfly::link::sysproxy
