#pragma once

// Which proxy the cloud link and every other HTTPS request of fairyfly uses, from all the places it can be set
// (docs/LINK.md "Corporate proxies"). The first source that decides wins:
//
//   1. fairyfly policy      HKLM\SOFTWARE\Policies\DataZoo\fairyfly ProxyMode (+ ProxyServer, ProxyPacUrl, ProxyBypass):
//                           locks the setting (the tray window shows "set by your administrator", the CLI refuses);
//   2. your fairyfly setting `fairyfly proxy set` / the tray's "Network / proxy..." (HKCU tray settings);
//   3. HTTPS_PROXY          (with NO_PROXY), the environment of the process;
//   4. browser policy       Edge, then Chrome (HKLM, else HKCU \Software\Policies\Microsoft\Edge | Google\Chrome), then
//                           Firefox (HKLM\Software\Policies\Mozilla\Firefox\Proxy, else distribution\policies.json);
//                           the first browser with a proxy policy decides, "system" hands over to 5;
//   5. Windows              the user's PAC script / automatic detection (WPAD) / manual proxy, then netsh winhttp
//                           (system_proxy.h resolve()).
// A setting in mode Auto (1 or 2) decides nothing: the next sources decide (with 1 in Auto, 2 is skipped).
//
// Trust (which credentials a proxy may get) follows from where it came from, as before (system_proxy.h): a proxy named
// explicitly by 1, 2, 3 or a browser policy is Origin::configured (Windows login and a stored Basic credential); a PAC
// script's proxies are Origin::pac when the script is trusted (https://, or an http:// dotted non-IP host set by an
// administrator: 1 or a browser policy that only administrators can write), else Origin::discovered like WPAD's.
// A Firefox policies.json that a non-administrator can change counts as the user's own setting, not as policy.
//
// Everything here is portable (unit-tested, and end to end against a real squid on Linux); the registry, file ACL and
// WinHTTP parts are in proxy_sources_win32.cpp and system_proxy_win32.cpp.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "include/link/system_proxy.h"
#include "include/tray/tray.h"  // the names of the user's setting (kTraySettingNames)
#include "proxy.hpp"  // libffl

namespace fairyfly::link::proxycfg {

// ---- the fairyfly setting -------------------------------------------------------------------------------------

enum class Mode { automatic, manual, pac, direct };
/// "auto", "manual", "pac", "direct"
std::string mode_name(Mode m);
/// Case-insensitive; nullopt for anything else.
std::optional<Mode> parse_mode(const std::string& s);

struct Setting {
    Mode mode = Mode::automatic;
    std::string server;   ///< manual: host:port (normalised by validate)
    std::string bypass;   ///< manual: ';'-separated exceptions, IE syntax (<local>, *.corp.example, 10.*)
    std::string pac_url;  ///< pac: http:// or https://
    bool operator==(const Setting&) const = default;
};

/// The checks every setting passes before it is saved or used (policy values too). nullopt = fine, else the reason,
/// without echoing the value. Rules: no control characters, spaces or CR/LF anywhere; the proxy is host:port (a leading
/// http:// is accepted and dropped; https:// = TLS to the proxy, socks:// and other schemes are refused, as are
/// user:password@ and paths); the PAC URL is http:// or https:// with a host; bypass entries are printable.
std::optional<std::string> check_server(const std::string& server);
std::optional<std::string> check_pac_url(const std::string& url);
std::optional<std::string> check_bypass(const std::string& bypass);
/// The mode's required fields are set and valid; the other fields are ignored (and dropped by normalised()).
std::optional<std::string> check(const Setting& s);
/// The setting as it is stored and compared: server without http://, bypass separated by ';', unused fields empty.
Setting normalised(const Setting& s);

/// The administrator's setting (HKLM\SOFTWARE\Policies\DataZoo\fairyfly). Present as soon as ProxyMode exists: then
/// the setting is locked, also when its values are invalid (error says why; the proxy is then chosen as in Auto).
struct PolicySetting {
    bool present = false;
    Setting setting;
    std::string error;
};
/// From the raw registry values (nullopt = value absent).
PolicySetting policy_from_values(const std::optional<std::string>& mode, const std::optional<std::string>& server,
                                 const std::optional<std::string>& pac_url, const std::optional<std::string>& bypass);
inline constexpr const wchar_t* kPolicyModeValue = L"ProxyMode";
inline constexpr const wchar_t* kPolicyServerValue = L"ProxyServer";
inline constexpr const wchar_t* kPolicyPacValue = L"ProxyPacUrl";
inline constexpr const wchar_t* kPolicyBypassValue = L"ProxyBypass";

/// The user's setting, in the trays' per-user settings (tray::TraySettings, the key tray::contract::kSettingsKey).
inline constexpr const char* kUserModeSetting = tray::kProxyModeSetting;
inline constexpr const char* kUserServerSetting = tray::kProxyServerSetting;
inline constexpr const char* kUserBypassSetting = tray::kProxyBypassSetting;
inline constexpr const char* kUserPacSetting = tray::kProxyPacSetting;
using SettingGet = std::function<std::optional<std::string>(const std::string& name)>;
using SettingSet = std::function<bool(const std::string& name, const std::string& value)>;
/// nullopt: none stored (or "auto"), or what is stored does not pass check() (warning says why; it is then ignored).
std::optional<Setting> load_user_setting(const SettingGet& get, std::string* warning = nullptr);
/// Writes all four values (Auto clears the others). False when the store refused a write.
bool save_user_setting(const SettingSet& set, const Setting& s);

// ---- browser policies -------------------------------------------------------------------------------------------

struct BrowserPolicy {
    enum class Kind { direct, system, auto_detect, pac, fixed };
    std::string browser;   ///< "Edge", "Chrome", "Firefox"
    std::string where;     ///< "HKLM\\SOFTWARE\\Policies\\Microsoft\\Edge", "C:\\...\\distribution\\policies.json"
    bool admin = true;     ///< only administrators can change it (registry policy keys; a policies.json with such an ACL)
    Kind kind = Kind::system;
    std::string server;    ///< fixed: a proxy list (WinHTTP/Chrome syntax: "host:port", "https=a:1;http=b:2")
    std::string bypass;
    std::string pac_url;
    std::string label() const;  ///< "Edge policy (HKLM)", "Firefox policies.json (user-writable)"
};

/// The Chrome/Edge proxy policies as stored (REG_SZ, ProxyServerMode a DWORD). The ProxySettings policy (a JSON
/// dictionary with the same names) overrides the single policies, as in Chrome.
struct ChromiumValues {
    std::optional<std::string> mode, server, pac_url, bypass, settings_json;
    std::optional<std::uint32_t> server_mode;  ///< legacy ProxyServerMode: 0 direct, 1 auto detect, 2 manual, 3 system
};
/// nullopt: no proxy policy (or an unusable one: `warning` says why).
std::optional<BrowserPolicy> chromium_policy(const std::string& browser, const std::string& where, const ChromiumValues& v,
                                             std::string* warning = nullptr);

/// Firefox's Proxy policy (registry values under ...\Mozilla\Firefox\Proxy, or "policies"."Proxy" of policies.json).
struct FirefoxValues {
    std::optional<std::string> mode, http_proxy, ssl_proxy, autoconfig_url, passthrough;
    std::optional<bool> http_for_all;  ///< UseHTTPProxyForAllProtocols
    bool any() const { return mode || http_proxy || ssl_proxy || autoconfig_url || passthrough || http_for_all; }
};
/// From the text of a policies.json; error set when it is not JSON or "Proxy" is not an object.
FirefoxValues firefox_values_from_json(const std::string& text, std::string* error = nullptr);
std::optional<BrowserPolicy> firefox_policy(const std::string& where, bool admin, const FirefoxValues& v,
                                            std::string* warning = nullptr);

/// Who may change a file or folder, as its security descriptor says (proxy_sources_win32.cpp reads it).
struct Ace {
    bool allow = true;
    bool inherit_only = false;   ///< applies to children only
    bool admin_like = false;     ///< SYSTEM, Administrators, TrustedInstaller (or CREATOR OWNER on an inherit-only ACE)
    std::uint32_t mask = 0;      ///< the access mask (generic rights not yet mapped)
};
struct SecurityInfo {
    bool readable = false;   ///< the descriptor could be read
    bool owner_admin_like = false;
    bool has_dacl = false;   ///< no DACL = everyone has full access
    std::vector<Ace> aces;
};
/// True only when nobody but administrators (SYSTEM, TrustedInstaller) can write, delete, re-permission or take it;
/// for a folder also create or delete its entries. Deny entries are not counted (fail closed).
bool only_admins_can_change(const SecurityInfo& info, bool directory);

// ---- the decision -------------------------------------------------------------------------------------------------

/// HTTPS_PROXY / NO_PROXY of the process, taken at start (take_env_overrides): fairyfly ranks them itself, below the
/// fairyfly setting, so libffl must not see them first.
struct EnvOverrides {
    std::string https_proxy;  ///< raw value (may hold user:password@; never logged)
    std::string no_proxy;
};
/// Reads HTTPS_PROXY and NO_PROXY and hides them from libffl's lookup (config::take_from_runtime_view: the C
/// runtime's copy; the process environment block keeps them, so child processes inherit them). Windows
/// (proxy_sources_win32.cpp): install_system_proxy() calls it once at start.
EnvOverrides take_env_overrides();

enum class Tier { fairyfly_policy, user_setting, environment, browser_policy, windows };
/// "fairyfly policy", "your fairyfly setting", "HTTPS_PROXY", "browser policy", "Windows proxy settings"
std::string tier_name(Tier t);

struct Inputs {
    PolicySetting policy;
    std::optional<Setting> user;
    EnvOverrides env;
    std::vector<BrowserPolicy> browsers;       ///< in order (Edge, Chrome, Firefox); at most one per browser
    sysproxy::ProxySettings windows;           ///< the user's Windows settings
    std::optional<sysproxy::ProxySettings> machine;  ///< netsh winhttp
    bool discovered_login_policy = false;      ///< AllowLoginToDiscoveredProxies (for the trust shown)
    std::vector<std::string> warnings;         ///< from reading the sources (shown by `proxy show` and doctor)
};

/// Evaluates a proxy script for `url` (src = pac: `pac_url`; src = wpad: detection). `trusted`: the script may be
/// answered with the Windows login when its server asks (system_proxy.h script_logon_allowed).
using AutoProxy = std::function<sysproxy::AutoProxyResult(const std::string& url, sysproxy::AutoSource src, const std::string& pac_url,
                                                          bool trusted)>;

struct RouteDecision {
    ffy::proxy::Routes routes;  ///< empty = DIRECT
    Tier tier = Tier::windows;
    std::string source;         ///< tier_name, with the browser policy's label ("Edge policy (HKLM)")
    std::string how;            ///< what decided within it: "manual proxy", "PAC https://pac/p.pac", "bypass list", ...
    std::string pac_url;        ///< the script that decided, without its query ("" when none did)
    std::string error;          ///< the route cannot be used (an invalid HTTPS_PROXY): requests fail with it
    // the setting as the UI shows it
    bool locked = false;        ///< the fairyfly policy is present
    std::string policy_error;
    Setting effective;          ///< the fairyfly policy's, else the user's, else Auto
    std::string setting_from;   ///< "policy", "user", "default"
    bool discovered_login_policy = false;  ///< Inputs::discovered_login_policy, for route_text
};

/// The routes for an https:// URL (the order at the top of this file).
RouteDecision decide(const Inputs& in, const std::string& url, const AutoProxy& autoproxy);

/// Whether the first proxy of the decision may get the Windows login / a stored Basic credential.
bool windows_login(const RouteDecision& d, bool discovered_login_policy);
bool stored_basic_allowed(const RouteDecision& d);
/// The route in plain words: "via proxy.corp:8080 from PAC (https://pac.corp/p.pac, Edge policy (HKLM)), Windows
/// login: yes" or "direct (your fairyfly setting: Direct)".
std::string route_text(const RouteDecision& d, bool discovered_login_policy);
nlohmann::json to_json(const RouteDecision& d, bool discovered_login_policy);

/// For the duration of its life on this thread, the resolver uses `candidate` in place of the saved user setting
/// (the tray window's and `proxy test`'s "Test connection" of a setting not saved yet). Nests; not shared with other
/// threads, so the running link is not affected.
class ScopedCandidate {
public:
    explicit ScopedCandidate(Setting candidate);
    ~ScopedCandidate();
    ScopedCandidate(const ScopedCandidate&) = delete;
    ScopedCandidate& operator=(const ScopedCandidate&) = delete;
    static const Setting* current();

private:
    Setting candidate_;
    const Setting* previous_;
};

/// The process's resolver for libffl: routes per scheme://host:port cached for 5 minutes (automatic detection can take
/// seconds, the link asks on every reconnect), dropped at once when the fingerprint of the fairyfly setting changes.
class RouteResolver {
public:
    struct Sources {
        std::function<Inputs()> inputs;
        AutoProxy autoproxy;
        /// cheap: the raw fairyfly policy + user setting; a change empties the cache
        std::function<std::string()> fingerprint;
    };
    explicit RouteResolver(Sources s);
    /// For libffl (Config::resolve): cached; with a ScopedCandidate on this thread uncached and with the candidate.
    ffy::proxy::Routes routes(const std::string& url);
    /// Uncached, with everything that decided (`proxy show`, doctor, the tray window).
    RouteDecision decision(const std::string& url, const std::optional<Setting>& candidate = std::nullopt);
    void invalidate();
    /// The current fingerprint differs from the one of the last call (the first call only records it): then the cache
    /// is emptied too. The running link polls it to reconnect when the setting changes.
    bool setting_changed();
    const Sources& sources() const { return src_; }

private:
    RouteDecision decide_now(const std::string& url, const std::optional<Setting>& candidate);
    Sources src_;
    std::mutex mu_;
    struct Cached {
        std::int64_t at_ms;
        std::string fingerprint;
        RouteDecision d;
    };
    std::map<std::string, Cached> cache_;
    std::map<std::string, std::string> logged_;
    std::optional<std::string> last_fingerprint_;
};

/// libffl's configuration around a resolver: routes from it, the authenticators by origin (system_proxy.h), and Basic
/// with HTTPS_PROXY's user:password for the HTTPS_PROXY proxy itself. Throws from resolve when the decision has an
/// error (an invalid HTTPS_PROXY that decides), so the request fails with it rather than going elsewhere.
struct AuthSources {
    std::function<bool()> discovered_login_policy;
    std::function<std::unique_ptr<ffy::proxy::Authenticator>(const ffy::proxy::Endpoint&)> sspi;   ///< nullptr: none
    std::function<std::optional<sysproxy::StoredLogin>(const std::string& host)> stored_login;    ///< nullptr: none
};
ffy::proxy::Config make_libffl_config(std::shared_ptr<RouteResolver> resolver, const EnvOverrides& env, AuthSources auth);
inline constexpr const char* kConfigSource = "the proxy settings (fairyfly proxy show)";

/// The resolver install_system_proxy() installed for this process (nullptr before, and outside Windows unless a test
/// set one).
std::shared_ptr<RouteResolver> process_resolver();
void set_process_resolver(std::shared_ptr<RouteResolver> r);

// ---- Test connection -----------------------------------------------------------------------------------------------

/// What "Test connection" reaches: the gateway's HTTPS (GET /health) and the link's WebSocket (an upgrade to the link
/// address, closed again at once; the gateway authenticates inside it, so nothing is sent).
struct ProbeTargets {
    std::string https_url;  ///< https://<host>/health
    std::string wss_url;    ///< the link address of the pairing, else wss://<gateway host>/ffl
    std::string note;       ///< e.g. "not paired: the link address the gateway hands out at pairing may differ"
};
/// From the pairing's link address (empty when not paired) and the gateway's web address (when not paired).
ProbeTargets probe_targets(const std::string& link_url, const std::string& gateway);

struct ProbeResult {
    bool ok = false;
    std::string route;      ///< route_text for the HTTPS target
    std::string https;      ///< "ok (HTTP 200)" or the error
    std::string websocket;  ///< "ok (upgraded)" or the error
    std::string summary;    ///< one paragraph for the window / the CLI
    nlohmann::json to_json() const;
};
/// Runs both steps through libffl (the process's proxy configuration, the ScopedCandidate of this thread). Blocks up
/// to about 2 x timeout_ms per step.
ProbeResult probe(const ProbeTargets& t, int timeout_ms, const std::function<RouteDecision(const std::string&)>& decision,
                  bool discovered_login_policy);

}  // namespace fairyfly::link::proxycfg
