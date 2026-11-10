#pragma once

// The Windows proxy settings for the cloud link and every other HTTPS request fairyfly makes through libffl
// (pairing, the link's WebSocket, update checks). libffl tunnels through the proxy (CONNECT) and asks the
// authenticators installed here; HTTPS_PROXY / NO_PROXY still override (libffl, third_party/libffl/src/proxy.hpp).
//
// Where the routes come from, in the order Windows itself uses:
//   1. automatic configuration of the current user (WinHttpGetIEProxyConfigForCurrentUser): "Automatically detect
//      settings" (WPAD) and/or a setup script (PAC URL), evaluated by WinHttpGetProxyForUrl for the URL;
//   2. if that is off or fails: the user's manual proxy server with its bypass list ("Don't use the proxy server for
//      local addresses" = <local>);
//   3. else WinHTTP's machine setting (netsh winhttp set proxy);
//   4. else DIRECT.
// Proxy authentication: Negotiate (Kerberos) or NTLM with the logged-on user's Windows credentials (SSPI), Basic only
// when a credential is stored explicitly for that proxy (Credential Manager, generic target "fairyfly-proxy:<host>",
// e.g. `cmdkey /generic:fairyfly-proxy:proxy.corp.example /user:CORP\alice /pass`).
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

/// The routes for an https:// URL from the settings, in Windows' order (see the top of this file). `autoproxy` is
/// called only when the user's settings ask for WPAD or a PAC script.
Resolution resolve(const ProxySettings& user, const std::optional<ProxySettings>& machine, const std::string& url,
                   const std::function<AutoProxyResult()>& autoproxy);

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

/// Installs the Windows resolver and the authenticators into libffl (once, at start). No-op outside Windows.
void install_system_proxy();

/// A Negotiate/NTLM authenticator with the logged-on user's credentials (SSPI). Windows only; nullptr elsewhere.
std::unique_ptr<ffy::proxy::Authenticator> make_sspi_authenticator(const std::string& proxy_host);

}  // namespace fairyfly::link::sysproxy
