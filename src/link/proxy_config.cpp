// Where the proxy comes from (include/link/proxy_config.h): the fairyfly policy and setting, HTTPS_PROXY, the browser
// policies and the Windows settings, in that order; validation of what is typed or set by policy; the cached resolver
// libffl asks; "Test connection". Portable: the registry and file readers are in proxy_sources_win32.cpp.
#include "include/link/proxy_config.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <cstdlib>

#include "http_client.hpp"  // libffl
#include "proto.hpp"        // libffl: the link's subprotocol
#include "util.hpp"         // libffl
#include "ws_client.hpp"    // libffl

namespace fairyfly::link::proxycfg {

using nlohmann::json;
using sysproxy::AutoProxyResult;
using sysproxy::AutoSource;

// ---- modes and checks ---------------------------------------------------------------------------------------------

std::string mode_name(Mode m) {
    switch (m) {
    case Mode::automatic: return "auto";
    case Mode::manual: return "manual";
    case Mode::pac: return "pac";
    case Mode::direct: return "direct";
    }
    return "auto";
}

std::optional<Mode> parse_mode(const std::string& s) {
    const std::string l = ffy::lower(ffy::trim(s));
    if (l == "auto" || l == "automatic") return Mode::automatic;
    if (l == "manual") return Mode::manual;
    if (l == "pac") return Mode::pac;
    if (l == "direct") return Mode::direct;
    return std::nullopt;
}

namespace {

bool has_control_or_space(const std::string& s) {
    return std::any_of(s.begin(), s.end(), [](unsigned char c) { return c <= 0x20 || c == 0x7f; });
}

bool host_ok(const std::string& h) {
    if (h.empty() || h.size() > 253) return false;
    if (h.front() == '[') {
        if (h.size() < 3 || h.back() != ']') return false;
        return std::all_of(h.begin() + 1, h.end() - 1, [](unsigned char c) { return std::isxdigit(c) || c == ':' || c == '.'; });
    }
    if (h.front() == '-' || h.front() == '.') return false;
    return std::all_of(h.begin(), h.end(), [](unsigned char c) { return std::isalnum(c) || c == '.' || c == '-' || c == '_'; });
}

// host:port; "" when it is not one
std::string split_hostport(const std::string& v, std::string& host, std::string& port) {
    if (!v.empty() && v.front() == '[') {
        auto close = v.find(']');
        if (close == std::string::npos || close + 1 >= v.size() || v[close + 1] != ':') return "a proxy address is host:port";
        host = v.substr(0, close + 1);
        port = v.substr(close + 2);
    } else {
        auto colon = v.rfind(':');
        if (colon == std::string::npos) return "the proxy address needs a port (host:port, e.g. proxy.corp.example:8080)";
        host = v.substr(0, colon);
        port = v.substr(colon + 1);
        if (host.find(':') != std::string::npos) return "an IPv6 proxy address is written [address]:port";
    }
    return {};
}

std::string strip_http(const std::string& v) {
    return ffy::lower(v.substr(0, 7)) == "http://" ? v.substr(7) : v;
}

}  // namespace

static bool has_control(const std::string& s) {
    return std::any_of(s.begin(), s.end(), [](unsigned char c) { return c < 0x20 || c == 0x7f; });
}

std::optional<std::string> check_server(const std::string& server_in) {
    if (has_control(server_in)) return "the proxy address contains control characters (line breaks, tabs)";
    const std::string server = ffy::trim(server_in);
    if (server.empty()) return "no proxy address (host:port)";
    if (server.size() > 300) return "the proxy address is too long";
    if (has_control_or_space(server)) return "the proxy address contains spaces or control characters";
    std::string v = server;
    if (auto s = v.find("://"); s != std::string::npos) {
        const std::string scheme = ffy::lower(v.substr(0, s));
        if (scheme == "https") return "https:// proxies (TLS to the proxy) are not supported: use the proxy's http:// address (host:port)";
        if (scheme.rfind("socks", 0) == 0) return "SOCKS proxies are not supported: use an HTTP proxy (host:port)";
        if (scheme != "http") return "only HTTP proxies are supported (host:port)";
        v = v.substr(s + 3);
    }
    while (!v.empty() && v.back() == '/') v.pop_back();
    if (v.find('@') != std::string::npos)
        return "no user name or password in the proxy address: the Windows login is used, or a Credential Manager entry fairyfly-proxy:<host>";
    if (v.find_first_of("/?#;=,\\") != std::string::npos) return "a proxy address is host:port, without path, list or scheme entries";
    std::string host, port;
    if (auto why = split_hostport(v, host, port); !why.empty()) return why;
    if (!host_ok(host)) return "the proxy host name is not valid";
    if (port.empty() || port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos) return "the proxy port is not a number";
    const int p = std::stoi(port);
    if (p < 1 || p > 65535) return "the proxy port must be between 1 and 65535";
    return std::nullopt;
}

std::optional<std::string> check_pac_url(const std::string& url_in) {
    if (has_control(url_in)) return "the PAC script address contains control characters (line breaks, tabs)";
    const std::string url = ffy::trim(url_in);
    if (url.empty()) return "no PAC script address";
    if (url.size() > 2048) return "the PAC script address is too long";
    if (has_control_or_space(url)) return "the PAC script address contains spaces or control characters";
    ffy::Url u;
    try {
        u = ffy::parse_url(url);
    } catch (const std::exception&) {
        return "the PAC script address is not a valid http:// or https:// URL";
    }
    if (u.scheme != "http" && u.scheme != "https") return "the PAC script address must start with http:// or https://";
    if (u.host.empty() || !host_ok(u.host)) return "the PAC script address has no valid host";
    return std::nullopt;
}

std::optional<std::string> check_bypass(const std::string& bypass) {
    if (bypass.size() > 4096) return "the exception list is too long";
    for (unsigned char c : bypass)
        if (c < 0x20 || c == 0x7f || c == '"' || c == '\'' || c == '\\' || c > 0x7e)
            if (!(c == '\t')) return "the exception list contains characters that are not allowed (control characters, quotes, backslash, non-ASCII)";
    return std::nullopt;
}

std::optional<std::string> check(const Setting& s) {
    switch (s.mode) {
    case Mode::automatic:
    case Mode::direct: return std::nullopt;
    case Mode::manual:
        if (auto e = check_server(s.server)) return e;
        return check_bypass(s.bypass);
    case Mode::pac: return check_pac_url(s.pac_url);
    }
    return std::nullopt;
}

Setting normalised(const Setting& s) {
    Setting n;
    n.mode = s.mode;
    if (s.mode == Mode::manual) {
        std::string v = strip_http(ffy::trim(s.server));
        while (!v.empty() && v.back() == '/') v.pop_back();
        n.server = v;
        std::string b;
        for (auto& t : ffy::split_csv([&] {
                 std::string x = s.bypass;
                 for (char& c : x)
                     if (c == ';' || c == ' ' || c == '\t') c = ',';
                 return x;
             }()))
            b += (b.empty() ? "" : ";") + t;
        n.bypass = b;
    }
    if (s.mode == Mode::pac) n.pac_url = ffy::trim(s.pac_url);
    return n;
}

PolicySetting policy_from_values(const std::optional<std::string>& mode, const std::optional<std::string>& server,
                                 const std::optional<std::string>& pac_url, const std::optional<std::string>& bypass) {
    PolicySetting p;
    if (!mode) return p;
    p.present = true;
    auto m = parse_mode(*mode);
    if (!m) {
        p.error = "ProxyMode must be auto, manual, pac or direct";
        return p;
    }
    Setting s;
    s.mode = *m;
    s.server = server.value_or("");
    s.pac_url = pac_url.value_or("");
    s.bypass = bypass.value_or("");
    if (auto e = check(s)) {
        p.error = (s.mode == Mode::manual ? "ProxyServer/ProxyBypass: " : "ProxyPacUrl: ") + *e;
        return p;
    }
    p.setting = normalised(s);
    return p;
}

std::optional<Setting> load_user_setting(const SettingGet& get, std::string* warning) {
    if (!get) return std::nullopt;
    auto mode = get(kUserModeSetting);
    if (!mode || mode->empty()) return std::nullopt;
    auto m = parse_mode(*mode);
    if (!m) {
        if (warning) *warning = "the saved proxy mode is not valid; it is ignored (fairyfly proxy reset)";
        return std::nullopt;
    }
    if (*m == Mode::automatic) return std::nullopt;
    Setting s;
    s.mode = *m;
    s.server = get(kUserServerSetting).value_or("");
    s.bypass = get(kUserBypassSetting).value_or("");
    s.pac_url = get(kUserPacSetting).value_or("");
    if (auto e = check(s)) {
        if (warning) *warning = "the saved proxy setting is not valid (" + *e + "); it is ignored (fairyfly proxy set or reset)";
        return std::nullopt;
    }
    return normalised(s);
}

bool save_user_setting(const SettingSet& set, const Setting& in) {
    if (!set) return false;
    const Setting s = normalised(in);
    bool ok = true;
    ok = set(kUserServerSetting, s.server) && ok;
    ok = set(kUserBypassSetting, s.bypass) && ok;
    ok = set(kUserPacSetting, s.pac_url) && ok;
    ok = set(kUserModeSetting, s.mode == Mode::automatic ? std::string() : mode_name(s.mode)) && ok;  // last: a reader sees all
    return ok;
}

// ---- browser policies ---------------------------------------------------------------------------------------------

std::string BrowserPolicy::label() const {
    std::string scope;
    if (where.rfind("HKLM", 0) == 0) scope = "HKLM";
    else if (where.rfind("HKCU", 0) == 0) scope = "HKCU";
    if (browser == "Firefox" && scope.empty()) return std::string("Firefox policies.json") + (admin ? "" : " (user-writable)");
    return browser + " policy" + (scope.empty() ? "" : " (" + scope + ")");
}

namespace {

void warn(std::string* w, const std::string& text) {
    if (w) *w = text;
}

// Chrome's proxy server list: "host:port", "http://host:port", "https=a:1;http=b:2". Kept for parse_proxy_list.
bool usable_server_list(const std::string& s) { return !sysproxy::parse_proxy_list(s).empty(); }

}  // namespace

std::optional<BrowserPolicy> chromium_policy(const std::string& browser, const std::string& where, const ChromiumValues& in,
                                             std::string* warning) {
    ChromiumValues v = in;
    if (v.settings_json && !ffy::trim(*v.settings_json).empty()) {
        // ProxySettings overrides ProxyMode, ProxyServer, ProxyPacUrl, ProxyBypassList and ProxyServerMode
        json j = json::parse(*v.settings_json, nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
            warn(warning, browser + " policy ProxySettings is not a JSON object; ignored");
        } else {
            auto str = [&](const char* k) -> std::optional<std::string> {
                if (j.contains(k) && j[k].is_string()) return j[k].get<std::string>();
                return std::nullopt;
            };
            v = ChromiumValues{};
            v.mode = str("ProxyMode");
            v.server = str("ProxyServer");
            v.pac_url = str("ProxyPacUrl");
            v.bypass = str("ProxyBypassList");
            if (j.contains("ProxyServerMode") && j["ProxyServerMode"].is_number_unsigned()) v.server_mode = j["ProxyServerMode"].get<std::uint32_t>();
        }
    }
    BrowserPolicy p;
    p.browser = browser;
    p.where = where;
    p.admin = true;
    p.bypass = v.bypass.value_or("");
    std::string mode;
    if (v.mode) {
        mode = ffy::lower(ffy::trim(*v.mode));
    } else if (v.server_mode) {
        switch (*v.server_mode) {
        case 0: mode = "direct"; break;
        case 1: mode = "auto_detect"; break;
        case 2: mode = "fixed_servers"; break;
        case 3: mode = "system"; break;
        default: warn(warning, browser + " policy ProxyServerMode " + std::to_string(*v.server_mode) + " is unknown; ignored"); return std::nullopt;
        }
    } else {
        if (v.server || v.pac_url) warn(warning, browser + " proxy policy without ProxyMode; ignored (as by " + browser + ")");
        return std::nullopt;
    }
    if (mode == "direct") p.kind = BrowserPolicy::Kind::direct;
    else if (mode == "system") p.kind = BrowserPolicy::Kind::system;
    else if (mode == "auto_detect") p.kind = BrowserPolicy::Kind::auto_detect;
    else if (mode == "pac_script") {
        p.kind = BrowserPolicy::Kind::pac;
        p.pac_url = ffy::trim(v.pac_url.value_or(""));
        if (auto e = check_pac_url(p.pac_url)) {
            warn(warning, browser + " policy ProxyPacUrl: " + *e + "; the policy is ignored");
            return std::nullopt;
        }
    } else if (mode == "fixed_servers") {
        p.kind = BrowserPolicy::Kind::fixed;
        p.server = ffy::trim(v.server.value_or(""));
        if (has_control_or_space(p.server) || !usable_server_list(p.server)) {
            warn(warning, browser + " policy ProxyServer has no HTTP proxy for https:// (https:// and SOCKS proxies are not supported); the policy is ignored");
            return std::nullopt;
        }
    } else {
        warn(warning, browser + " policy ProxyMode \"" + mode.substr(0, 20) + "\" is unknown; ignored");
        return std::nullopt;
    }
    if (auto e = check_bypass(p.bypass)) {
        warn(warning, browser + " policy ProxyBypassList: " + *e + "; the list is ignored");
        p.bypass.clear();
    }
    return p;
}

FirefoxValues firefox_values_from_json(const std::string& text, std::string* error) {
    FirefoxValues v;
    json j = json::parse(text, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        if (error) *error = "policies.json is not valid JSON";
        return v;
    }
    if (!j.contains("policies") || !j["policies"].is_object() || !j["policies"].contains("Proxy")) return v;
    const json& p = j["policies"]["Proxy"];
    if (!p.is_object()) {
        if (error) *error = "policies.json: \"Proxy\" is not an object";
        return v;
    }
    auto str = [&](const char* k) -> std::optional<std::string> {
        if (p.contains(k) && p[k].is_string()) return p[k].get<std::string>();
        return std::nullopt;
    };
    v.mode = str("Mode");
    v.http_proxy = str("HTTPProxy");
    v.ssl_proxy = str("SSLProxy");
    v.autoconfig_url = str("AutoConfigURL");
    v.passthrough = str("Passthrough");
    if (p.contains("UseHTTPProxyForAllProtocols") && p["UseHTTPProxyForAllProtocols"].is_boolean())
        v.http_for_all = p["UseHTTPProxyForAllProtocols"].get<bool>();
    return v;
}

std::optional<BrowserPolicy> firefox_policy(const std::string& where, bool admin, const FirefoxValues& v, std::string* warning) {
    if (!v.mode) {
        if (v.any()) warn(warning, "Firefox proxy policy without Mode; ignored");
        return std::nullopt;
    }
    BrowserPolicy p;
    p.browser = "Firefox";
    p.where = where;
    p.admin = admin;
    p.bypass = v.passthrough.value_or("");
    const std::string mode = ffy::lower(ffy::trim(*v.mode));
    if (mode == "none") p.kind = BrowserPolicy::Kind::direct;
    else if (mode == "system") p.kind = BrowserPolicy::Kind::system;
    else if (mode == "autodetect") p.kind = BrowserPolicy::Kind::auto_detect;
    else if (mode == "autoconfig") {
        p.kind = BrowserPolicy::Kind::pac;
        p.pac_url = ffy::trim(v.autoconfig_url.value_or(""));
        if (auto e = check_pac_url(p.pac_url)) {
            warn(warning, "Firefox policy AutoConfigURL: " + *e + "; the policy is ignored");
            return std::nullopt;
        }
    } else if (mode == "manual") {
        // for https:// Firefox uses SSLProxy, or HTTPProxy with "Also use this proxy for HTTPS"; neither: direct
        std::string server = ffy::trim(v.ssl_proxy.value_or(""));
        if (server.empty() && v.http_for_all.value_or(false)) server = ffy::trim(v.http_proxy.value_or(""));
        if (server.empty()) {
            p.kind = BrowserPolicy::Kind::direct;
        } else {
            if (check_server(server)) {
                warn(warning, "Firefox policy SSLProxy/HTTPProxy is not an HTTP proxy host:port; the policy is ignored");
                return std::nullopt;
            }
            p.kind = BrowserPolicy::Kind::fixed;
            p.server = strip_http(server);
        }
    } else {
        warn(warning, "Firefox policy Mode \"" + mode.substr(0, 20) + "\" is unknown; ignored");
        return std::nullopt;
    }
    if (auto e = check_bypass(p.bypass)) {
        warn(warning, "Firefox policy Passthrough: " + *e + "; the list is ignored");
        p.bypass.clear();
    }
    return p;
}

bool only_admins_can_change(const SecurityInfo& info, bool directory) {
    if (!info.readable || !info.has_dacl || !info.owner_admin_like) return false;
    // FILE_WRITE_DATA/ADD_FILE 0x2, FILE_APPEND_DATA/ADD_SUBDIRECTORY 0x4, FILE_WRITE_EA 0x10, FILE_DELETE_CHILD 0x40,
    // FILE_WRITE_ATTRIBUTES 0x100, DELETE 0x10000, WRITE_DAC 0x40000, WRITE_OWNER 0x80000, MAXIMUM_ALLOWED 0x2000000,
    // GENERIC_ALL 0x10000000, GENERIC_WRITE 0x40000000
    std::uint32_t risky = 0x2u | 0x4u | 0x10000u | 0x40000u | 0x80000u | 0x2000000u | 0x10000000u | 0x40000000u;
    if (directory) risky |= 0x40u;
    for (const auto& a : info.aces) {
        if (!a.allow || a.inherit_only || a.admin_like) continue;
        if (a.mask & risky) return false;
    }
    return true;
}

// ---- the decision -------------------------------------------------------------------------------------------------

std::string tier_name(Tier t) {
    switch (t) {
    case Tier::fairyfly_policy: return "fairyfly policy";
    case Tier::user_setting: return "your fairyfly setting";
    case Tier::environment: return "HTTPS_PROXY";
    case Tier::browser_policy: return "browser policy";
    case Tier::windows: return "Windows proxy settings";
    }
    return "";
}

namespace {

std::string without_query(const std::string& url) {
    auto q = url.find_first_of("?#");
    return q == std::string::npos ? url : url.substr(0, q);
}

ffy::proxy::Routes with_origin(ffy::proxy::Routes routes, ffy::proxy::Origin o) {
    for (auto& r : routes) r.proxy.origin = o;
    return routes;
}

struct Target {
    std::string host;
    int port = 443;
};

// A PAC script (or WPAD) for one tier: its answer, or DIRECT with the reason when it fails.
RouteDecision from_script(RouteDecision d, const Target& t, const std::string& url, AutoSource src, const std::string& pac_url, bool trusted,
                          const AutoProxy& autoproxy) {
    using ffy::proxy::Origin;
    const std::string what = src == AutoSource::pac ? "PAC " + without_query(pac_url) : std::string("automatic detection (WPAD)");
    if (src == AutoSource::pac) d.pac_url = without_query(pac_url);
    AutoProxyResult r = autoproxy ? autoproxy(url, src, pac_url, trusted) : AutoProxyResult{false, false, {}, {}, "not available"};
    if (!r.ok) {
        d.how = what + " failed (" + r.error + "): DIRECT";
        return d;
    }
    if (r.direct) {
        d.how = what + ": DIRECT";
        return d;
    }
    if (sysproxy::bypassed(t.host, t.port, r.bypass)) {
        d.how = what + ": bypass list";
        return d;
    }
    const Origin o = src == AutoSource::wpad ? Origin::discovered : (trusted ? Origin::pac : Origin::discovered);
    d.routes = with_origin(sysproxy::parse_proxy_list(r.proxy), o);
    d.how = d.routes.empty() ? what + ": DIRECT" : what;
    return d;
}

std::optional<RouteDecision> from_setting(RouteDecision d, const Setting& s, bool by_admin, const Target& t, const std::string& url,
                                          const AutoProxy& autoproxy) {
    switch (s.mode) {
    case Mode::automatic: return std::nullopt;
    case Mode::direct: d.how = "Direct"; return d;
    case Mode::manual:
        if (sysproxy::bypassed(t.host, t.port, s.bypass)) {
            d.how = "manual proxy: bypass list";
            return d;
        }
        d.routes = with_origin(sysproxy::parse_proxy_list(s.server), ffy::proxy::Origin::configured);
        d.how = "manual proxy";
        return d;
    case Mode::pac: return from_script(d, t, url, AutoSource::pac, s.pac_url, sysproxy::pac_trusted(s.pac_url, by_admin), autoproxy);
    }
    return std::nullopt;
}

}  // namespace

RouteDecision decide(const Inputs& in, const std::string& url, const AutoProxy& autoproxy) {
    using ffy::proxy::Origin;
    RouteDecision base;
    base.discovered_login_policy = in.discovered_login_policy;
    base.locked = in.policy.present;
    base.policy_error = in.policy.error;
    if (in.policy.present && in.policy.error.empty()) {
        base.effective = in.policy.setting;
        base.setting_from = "policy";
    } else if (!in.policy.present && in.user) {
        base.effective = *in.user;
        base.setting_from = "user";
    } else {
        base.setting_from = in.policy.present ? "policy" : "default";
    }
    const ffy::Url u = ffy::parse_url(url);
    const Target t{u.host, std::stoi(u.port)};

    // 1. the fairyfly policy (in Auto, or invalid, it skips the user setting too)
    if (in.policy.present) {
        RouteDecision d = base;
        d.tier = Tier::fairyfly_policy;
        d.source = tier_name(d.tier);
        if (in.policy.error.empty())
            if (auto r = from_setting(d, in.policy.setting, true, t, url, autoproxy)) return *r;
    } else if (in.user) {  // 2. the user's own
        RouteDecision d = base;
        d.tier = Tier::user_setting;
        d.source = tier_name(d.tier);
        if (auto r = from_setting(d, *in.user, false, t, url, autoproxy)) return *r;
    }
    // 3. the environment: NO_PROXY, HTTPS_PROXY
    if (!in.env.no_proxy.empty()) {
        std::string bare = t.host;
        if (bare.size() > 1 && bare.front() == '[' && bare.back() == ']') bare = bare.substr(1, bare.size() - 2);
        if (ffy::proxy_bypass(t.host, in.env.no_proxy) || ffy::proxy_bypass(bare, in.env.no_proxy)) {
            RouteDecision d = base;
            d.tier = Tier::environment;
            d.source = "NO_PROXY";
            return d;
        }
    }
    if (!ffy::trim(in.env.https_proxy).empty()) {
        RouteDecision d = base;
        d.tier = Tier::environment;
        d.source = tier_name(d.tier);
        d.how = "environment";
        try {
            auto e = ffy::proxy::parse_proxy_url(in.env.https_proxy);
            ffy::proxy::Endpoint ep = *e.proxy;
            ep.origin = Origin::configured;
            d.routes.push_back(ffy::proxy::Route::via(ep));
        } catch (const std::exception& x) {
            d.error = std::string("HTTPS_PROXY: ") + x.what();  // never the value: it may hold a password
        }
        return d;
    }
    // 4. browser policies: the first browser with one decides; "system" hands over to the Windows settings
    for (const auto& b : in.browsers) {
        RouteDecision d = base;
        d.tier = Tier::browser_policy;
        d.source = b.label();
        if (b.kind == BrowserPolicy::Kind::system) break;
        if (b.kind == BrowserPolicy::Kind::direct) {
            d.how = "Direct";
            return d;
        }
        if (b.kind == BrowserPolicy::Kind::auto_detect) return from_script(d, t, url, AutoSource::wpad, "", false, autoproxy);
        if (b.kind == BrowserPolicy::Kind::pac)
            return from_script(d, t, url, AutoSource::pac, b.pac_url, sysproxy::pac_trusted(b.pac_url, b.admin), autoproxy);
        // fixed
        if (sysproxy::bypassed(t.host, t.port, b.bypass)) {
            d.how = "proxy server: bypass list";
            return d;
        }
        d.routes = with_origin(sysproxy::parse_proxy_list(b.server), Origin::configured);
        d.how = "proxy server";
        return d;
    }
    // 5. Windows
    RouteDecision d = base;
    d.tier = Tier::windows;
    d.source = tier_name(d.tier);
    const sysproxy::ProxySettings& w = in.windows;
    const bool w_trusted = sysproxy::pac_trusted(w.auto_config_url, w.pac_by_policy);
    auto r = sysproxy::resolve(w, in.machine, url, [&](AutoSource src) {
        return autoproxy ? autoproxy(url, src, src == AutoSource::pac ? w.auto_config_url : std::string(), src == AutoSource::pac && w_trusted)
                         : AutoProxyResult{false, false, {}, {}, "not available"};
    });
    d.routes = r.routes;
    d.how = r.how;
    // the script decided (not: it failed and detection or the manual proxy did)
    if (r.how.rfind("PAC ", 0) == 0 && r.how.find(" failed (") == std::string::npos && !w.auto_config_url.empty())
        d.pac_url = without_query(w.auto_config_url);
    return d;
}

bool windows_login(const RouteDecision& d, bool discovered_login_policy) {
    for (const auto& r : d.routes)
        if (!r.direct) return sysproxy::windows_login_allowed(r.proxy.origin, discovered_login_policy);
    return false;
}

bool stored_basic_allowed(const RouteDecision& d) {
    for (const auto& r : d.routes)
        if (!r.direct) return r.proxy.origin == ffy::proxy::Origin::configured;
    return false;
}

namespace {
std::string first_proxy(const RouteDecision& d) {
    for (const auto& r : d.routes)
        if (!r.direct) return r.proxy.str();
    return {};
}
}  // namespace

std::string route_text(const RouteDecision& d, bool discovered_login_policy) {
    if (!d.error.empty()) return "unusable: " + d.error;
    const std::string proxy = first_proxy(d);
    if (proxy.empty()) {
        std::string why = d.source;
        if (!d.how.empty()) why += ": " + d.how;
        return "direct (" + why + ")";
    }
    std::string from;
    if (!d.pac_url.empty()) from = "PAC (" + d.pac_url + ", " + d.source + ")";
    else if (d.how.rfind("automatic detection", 0) == 0) from = "automatic detection (WPAD, " + d.source + ")";
    else from = d.source + (d.tier == Tier::windows && !d.how.empty() ? " (" + d.how + ")" : "");
    std::string more;
    if (d.routes.size() > 1) {
        more = ", then ";
        bool first = true;
        for (size_t i = 1; i < d.routes.size(); ++i) {
            more += (first ? "" : ", ") + (d.routes[i].direct ? std::string("direct") : d.routes[i].proxy.str());
            first = false;
        }
    }
    return "via " + proxy + more + " from " + from + ", Windows login: " + (windows_login(d, discovered_login_policy) ? "yes" : "no");
}

json to_json(const RouteDecision& d, bool discovered_login_policy) {
    json routes = json::array();
    for (const auto& r : d.routes) {
        if (r.direct) {
            routes.push_back("DIRECT");
            continue;
        }
        const char* o = r.proxy.origin == ffy::proxy::Origin::configured ? "configured"
                        : r.proxy.origin == ffy::proxy::Origin::pac      ? "pac"
                                                                         : "discovered";
        routes.push_back(json{{"proxy", r.proxy.str()}, {"origin", o}});
    }
    json j{{"route", route_text(d, discovered_login_policy)},
           {"routes", routes},
           {"source", d.source},
           {"tier", tier_name(d.tier)},
           {"how", d.how},
           {"windows_login", windows_login(d, discovered_login_policy)},
           {"stored_basic", stored_basic_allowed(d)}};
    if (!d.pac_url.empty()) j["pac_url"] = d.pac_url;
    if (!d.error.empty()) j["error"] = d.error;
    return j;
}

// ---- the resolver -------------------------------------------------------------------------------------------------

namespace {
thread_local const Setting* t_candidate = nullptr;

std::string route_key(const std::string& url) {
    const ffy::Url u = ffy::parse_url(url);
    return u.scheme + "://" + u.host + ":" + u.port;
}
}  // namespace

ScopedCandidate::ScopedCandidate(Setting candidate) : candidate_(normalised(candidate)), previous_(t_candidate) { t_candidate = &candidate_; }
ScopedCandidate::~ScopedCandidate() { t_candidate = previous_; }
const Setting* ScopedCandidate::current() { return t_candidate; }

RouteResolver::RouteResolver(Sources s) : src_(std::move(s)) {}

RouteDecision RouteResolver::decide_now(const std::string& url, const std::optional<Setting>& candidate) {
    Inputs in = src_.inputs ? src_.inputs() : Inputs{};
    if (candidate) in.user = candidate->mode == Mode::automatic ? std::nullopt : std::optional<Setting>(normalised(*candidate));
    return decide(in, url, src_.autoproxy);
}

RouteDecision RouteResolver::decision(const std::string& url, const std::optional<Setting>& candidate) { return decide_now(url, candidate); }

void RouteResolver::invalidate() {
    std::lock_guard<std::mutex> l(mu_);
    cache_.clear();
}

bool RouteResolver::setting_changed() {
    const std::string now = src_.fingerprint ? src_.fingerprint() : std::string();
    std::lock_guard<std::mutex> l(mu_);
    if (!last_fingerprint_) {
        last_fingerprint_ = now;
        return false;
    }
    if (*last_fingerprint_ == now) return false;
    last_fingerprint_ = now;
    cache_.clear();
    return true;
}


// Throws the decision's error (an invalid HTTPS_PROXY that decides), so that the request fails with it.
ffy::proxy::Routes RouteResolver::routes(const std::string& url) {
    if (const Setting* c = ScopedCandidate::current()) {
        RouteDecision d = decide_now(url, *c);
        if (!d.error.empty()) throw ffy::Error(d.error);
        return d.routes;
    }
    const std::string key = route_key(url);
    const std::string fp = src_.fingerprint ? src_.fingerprint() : std::string();
    const std::int64_t now = ffy::now_ms();
    {
        std::lock_guard<std::mutex> l(mu_);
        if (auto i = cache_.find(key); i != cache_.end() && now - i->second.at_ms < 5 * 60 * 1000 && i->second.fingerprint == fp) {
            if (!i->second.d.error.empty()) throw ffy::Error(i->second.d.error);
            return i->second.d.routes;
        }
    }
    RouteDecision d = decide_now(url, std::nullopt);  // outside the lock: detection can take seconds
    const ffy::Url u = ffy::parse_url(url);
    const std::string line = route_text(d, d.discovered_login_policy);
    {
        std::lock_guard<std::mutex> l(mu_);
        if (logged_[key] != line) {  // once per change, not on every reconnect
            spdlog::info("proxy: {} -> {}", u.host, line);
            logged_[key] = line;
        }
        cache_[key] = Cached{now, fp, d};
    }
    if (!d.error.empty()) throw ffy::Error(d.error);
    return d.routes;
}

ffy::proxy::Config make_libffl_config(std::shared_ptr<RouteResolver> resolver, const EnvOverrides& env, AuthSources auth) {
    ffy::proxy::Config c;
    c.source = kConfigSource;
    c.resolve = [resolver](const std::string& url) { return resolver->routes(url); };
    auto policy = auth.discovered_login_policy ? auth.discovered_login_policy : [] { return false; };
    auto sspi = auth.sspi;
    c.authenticators.push_back([policy, sspi](const ffy::proxy::Endpoint& e) -> std::unique_ptr<ffy::proxy::Authenticator> {
        return sysproxy::login_authenticator(e, policy(), [&]() -> std::unique_ptr<ffy::proxy::Authenticator> {
            return sspi ? sspi(e) : nullptr;
        });
    });
    if (auth.stored_login) {
        auto read = auth.stored_login;
        c.authenticators.push_back([read](const ffy::proxy::Endpoint& e) {
            return sysproxy::stored_basic(e, [read, host = e.host] { return read(host); });
        });
    }
    // HTTPS_PROXY's user:password, as libffl offers it for the environment's proxy: only to that proxy, only for Basic
    if (!ffy::trim(env.https_proxy).empty()) {
        try {
            auto e = ffy::proxy::parse_proxy_url(env.https_proxy);
            if (e.proxy && (!e.user.empty() || !e.password.empty())) {
                c.authenticators.push_back([ep = *e.proxy, user = e.user, pw = e.password](
                                               const ffy::proxy::Endpoint& px) -> std::unique_ptr<ffy::proxy::Authenticator> {
                    if (!(px == ep)) return nullptr;
                    return ffy::proxy::basic(user, pw);
                });
            }
        } catch (const std::exception&) {
            // reported by the decision when HTTPS_PROXY decides
        }
    }
    return c;
}

namespace {
std::mutex g_resolver_mu;
std::shared_ptr<RouteResolver>& resolver_slot() {
    static std::shared_ptr<RouteResolver> r;
    return r;
}
}  // namespace

std::shared_ptr<RouteResolver> process_resolver() {
    std::lock_guard<std::mutex> l(g_resolver_mu);
    return resolver_slot();
}

void set_process_resolver(std::shared_ptr<RouteResolver> r) {
    std::lock_guard<std::mutex> l(g_resolver_mu);
    resolver_slot() = std::move(r);
}


// ---- Test connection ------------------------------------------------------------------------------------------------

ProbeTargets probe_targets(const std::string& link_url, const std::string& gateway) {
    ProbeTargets t;
    if (!link_url.empty()) {
        const ffy::Url u = ffy::parse_url(link_url);
        const std::string hostport = u.host + ((u.port == "443" || u.port.empty()) ? "" : ":" + u.port);
        t.wss_url = link_url;
        t.https_url = "https://" + hostport + "/health";
        return t;
    }
    const ffy::Url g = ffy::parse_url(gateway);
    const std::string hostport = g.host + ((g.port == "443" || g.port.empty()) ? "" : ":" + g.port);
    t.https_url = "https://" + hostport + "/health";
    t.wss_url = "wss://" + hostport + "/ffl";
    t.note = "not paired yet: the link address the gateway hands out at pairing may be another host";
    return t;
}

json ProbeResult::to_json() const {
    return json{{"ok", ok}, {"route", route}, {"https", https}, {"websocket", websocket}, {"summary", summary}};
}

ProbeResult probe(const ProbeTargets& t, int timeout_ms, const std::function<RouteDecision(const std::string&)>& decision,
                  bool discovered_login_policy) {
    ProbeResult r;
    if (decision) {
        try {
            r.route = route_text(decision(t.https_url), discovered_login_policy);
        } catch (const std::exception& e) {
            r.route = std::string("cannot be determined: ") + e.what();
        }
    }
    bool https_ok = false, ws_ok = false;
    try {
        ffy::HttpRequest rq;
        rq.url = t.https_url;
        rq.timeout_ms = timeout_ms;
        rq.max_body = 64 * 1024;
        auto res = ffy::http_do(rq);
        https_ok = true;  // an answer of the gateway through the route, whatever its status
        r.https = "ok (HTTP " + std::to_string(res.status) + ")";
    } catch (const std::exception& e) {
        r.https = e.what();
    }
    try {
        auto ws = ffy::WsClient::connect(t.wss_url, ffy::proto::kSubprotocol, timeout_ms);
        ws->close();
        ws_ok = true;
        r.websocket = "ok (upgraded)";
    } catch (const std::exception& e) {
        r.websocket = e.what();
    }
    r.ok = https_ok && ws_ok;
    const ffy::Url hu = ffy::parse_url(t.https_url);
    if (r.ok) r.summary = "Connected to " + hu.host + " (HTTPS and the link's WebSocket) " + r.route + ".";
    else
        r.summary = "Could not connect to " + hu.host + ": " + (https_ok ? "" : "HTTPS: " + r.https + (ws_ok ? "" : "; ")) +
                    (ws_ok ? "" : "WebSocket: " + r.websocket) + ". Route: " + r.route + ".";
    if (!t.note.empty()) r.summary += " (" + t.note + ")";
    return r;
}

}  // namespace fairyfly::link::proxycfg
