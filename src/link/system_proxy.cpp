// The portable part of the Windows proxy settings (see include/link/system_proxy.h): proxy lists, bypass lists, the
// order of the settings and the choice of an SSPI challenge. The WinHTTP and SSPI calls are in system_proxy_win32.cpp.
#include "include/link/system_proxy.h"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cctype>
#include <mutex>
#include <set>

#include "util.hpp"  // libffl

namespace fairyfly::link::sysproxy {

namespace {

std::vector<std::string> tokens(const std::string& s, const std::string& seps) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (seps.find(c) != std::string::npos) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur.push_back(c);
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}

std::optional<ffy::proxy::Endpoint> endpoint(std::string v) {
    std::string l = ffy::lower(v);
    if (l.rfind("http://", 0) == 0) {
        v = v.substr(7);
    } else if (l.find("://") != std::string::npos) {  // https:// (TLS to the proxy), socks://: not supported, said once
        static std::mutex mu;
        static std::set<std::string> warned;
        std::lock_guard<std::mutex> g(mu);
        if (warned.insert(l).second)
            spdlog::warn("proxy: the proxy entry {} is skipped: only http:// proxies are supported (no TLS to the proxy, no SOCKS)",
                         l.substr(0, 120));
        return std::nullopt;
    }
    while (!v.empty() && v.back() == '/') v.pop_back();
    ffy::proxy::Endpoint e;
    std::string port;
    if (!v.empty() && v[0] == '[') {
        auto close = v.find(']');
        if (close == std::string::npos) return std::nullopt;
        e.host = v.substr(0, close + 1);
        if (close + 1 < v.size()) {
            if (v[close + 1] != ':') return std::nullopt;
            port = v.substr(close + 2);
        }
    } else {
        auto colon = v.rfind(':');
        e.host = v.substr(0, colon);
        if (colon != std::string::npos) port = v.substr(colon + 1);
    }
    if (e.host.empty()) return std::nullopt;
    for (unsigned char c : e.host)
        if (!(std::isalnum(c) || c == '.' || c == '-' || c == '_' || c == ':' || c == '[' || c == ']')) return std::nullopt;
    if (port.empty()) {
        e.port = 80;  // WinHTTP's default for a proxy without a port
    } else {
        if (port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos) return std::nullopt;
        e.port = std::stoi(port);
        if (e.port < 1 || e.port > 65535) return std::nullopt;
    }
    return e;
}

bool glob(const std::string& pat, const std::string& s) {
    // '*' matches any sequence (iterative, with backtracking to the last star)
    size_t p = 0, i = 0, star = std::string::npos, mark = 0;
    while (i < s.size()) {
        if (p < pat.size() && pat[p] == '*') {
            star = p++;
            mark = i;
        } else if (p < pat.size() && pat[p] == s[i]) {
            ++p;
            ++i;
        } else if (star != std::string::npos) {
            p = star + 1;
            i = ++mark;
        } else {
            return false;
        }
    }
    while (p < pat.size() && pat[p] == '*') ++p;
    return p == pat.size();
}

std::string without_query(const std::string& url) {
    auto q = url.find_first_of("?#");
    return q == std::string::npos ? url : url.substr(0, q);
}

}  // namespace

ffy::proxy::Routes parse_proxy_list(const std::string& list) {
    ffy::proxy::Routes https, plain;
    bool next_is_host = false;
    for (auto& t : tokens(list, "; \t\r\n")) {
        if (t == "DIRECT") {  // upper case, as WinHTTP and PAC write it (a host may be called "direct" or "proxy")
            https.push_back(ffy::proxy::Route{});
            plain.push_back(ffy::proxy::Route{});
            continue;
        }
        if (t == "PROXY") {  // PAC syntax ("PROXY host:port"), should a result carry it
            next_is_host = true;
            continue;
        }
        auto eq = t.find('=');
        if (eq != std::string::npos && !next_is_host) {
            std::string scheme = ffy::lower(t.substr(0, eq));
            if (scheme != "https") continue;  // http=, ftp=, socks=: not for an https:// URL
            if (auto e = endpoint(t.substr(eq + 1))) https.push_back(ffy::proxy::Route::via(*e));
            continue;
        }
        next_is_host = false;
        if (auto e = endpoint(t)) plain.push_back(ffy::proxy::Route::via(*e));
    }
    bool has_https = std::any_of(https.begin(), https.end(), [](const ffy::proxy::Route& r) { return !r.direct; });
    ffy::proxy::Routes out = has_https ? https : plain;
    // a list of nothing but DIRECT is DIRECT
    if (std::all_of(out.begin(), out.end(), [](const ffy::proxy::Route& r) { return r.direct; })) out.clear();
    return out;
}

bool bypassed(const std::string& host_in, int port, const std::string& bypass_list) {
    std::string host = ffy::lower(host_in);
    if (host.size() > 1 && host.front() == '[' && host.back() == ']') host = host.substr(1, host.size() - 2);
    const std::string hostport = host + ":" + std::to_string(port);
    for (auto p : tokens(bypass_list, ";, \t\r\n")) {
        p = ffy::lower(p);
        if (p == "<local>") {
            if (host.find('.') == std::string::npos && host.find(':') == std::string::npos) return true;
            continue;
        }
        if (p.front() == '<') continue;  // <-loopback> and the like
        if (auto s = p.find("://"); s != std::string::npos) p = p.substr(s + 3);
        if (p.empty()) continue;  // "http://" alone
        if (p.front() == '.') p = "*" + p;
        if (glob(p, host) || glob(p, hostport)) return true;
    }
    return false;
}

namespace {
ffy::proxy::Routes with_origin(ffy::proxy::Routes routes, ffy::proxy::Origin o) {
    for (auto& r : routes) r.proxy.origin = o;
    return routes;
}
}  // namespace

Resolution resolve(const ProxySettings& user, const std::optional<ProxySettings>& machine, const std::string& url,
                   const std::function<AutoProxyResult(AutoSource)>& autoproxy) {
    using ffy::proxy::Origin;
    ffy::Url u = ffy::parse_url(url);
    const int port = std::stoi(u.port);
    std::string note;
    auto automatic = [&](AutoSource src, const std::string& what, Origin origin) -> std::optional<Resolution> {
        AutoProxyResult r = autoproxy ? autoproxy(src) : AutoProxyResult{false, false, {}, {}, "not available"};
        if (!r.ok) {
            note += what + " failed (" + r.error + "), ";
            return std::nullopt;
        }
        if (r.direct) return Resolution{{}, note + what + ": DIRECT"};
        if (bypassed(u.host, port, r.bypass)) return Resolution{{}, note + what + ": bypass list"};
        auto routes = with_origin(parse_proxy_list(r.proxy), origin);
        return Resolution{routes, note + (routes.empty() ? what + ": DIRECT" : what)};
    };
    if (!user.auto_config_url.empty()) {
        const Origin o = pac_trusted(user.auto_config_url, user.pac_by_policy) ? Origin::pac : Origin::discovered;
        if (auto r = automatic(AutoSource::pac, "PAC " + without_query(user.auto_config_url), o)) return *r;
        if (user.auto_detect)
            spdlog::warn("proxy: {}falling back to automatic detection (WPAD); its proxies get no credentials", note);
    }
    if (user.auto_detect)
        if (auto r = automatic(AutoSource::wpad, "automatic detection (WPAD)", Origin::discovered)) return *r;
    if (!user.proxy.empty()) {
        if (bypassed(u.host, port, user.bypass)) return {{}, note + "manual proxy settings: bypass list"};
        auto routes = with_origin(parse_proxy_list(user.proxy), Origin::configured);
        return {routes, note + (routes.empty() ? "manual proxy settings: none for https" : "manual proxy settings")};
    }
    if (machine && !machine->proxy.empty()) {
        if (bypassed(u.host, port, machine->bypass)) return {{}, note + "WinHTTP machine setting: bypass list"};
        auto routes = with_origin(parse_proxy_list(machine->proxy), Origin::configured);
        return {routes, note + "WinHTTP machine setting (netsh winhttp)"};
    }
    return {{}, note + "no proxy configured"};
}

bool pac_trusted(const std::string& pac_url, bool by_policy) {
    ffy::Url u;
    try {
        u = ffy::parse_url(pac_url);
    } catch (const std::exception&) {
        return false;
    }
    if (u.scheme == "https") return true;  // the server proves its name: nobody on the LAN can stand in for it
    if (!by_policy || u.host.find('.') == std::string::npos) return false;
    // a dotted host name, not an address (an IP literal says nothing about who set it up)
    return std::any_of(u.host.begin(), u.host.end(), [](unsigned char c) { return std::isalpha(c); }) &&
           u.host.find(':') == std::string::npos;
}

namespace {
thread_local bool t_no_script_login = false;
}  // namespace

NoScriptLoginScope::NoScriptLoginScope() : previous_(t_no_script_login) { t_no_script_login = true; }
NoScriptLoginScope::~NoScriptLoginScope() { t_no_script_login = previous_; }
bool NoScriptLoginScope::active() { return t_no_script_login; }

bool script_logon_allowed(AutoSource src, bool pac_is_trusted, bool discovered_login_policy) {
    if (NoScriptLoginScope::active()) return false;  // an untrusted source's script: never, whatever the policy
    if (discovered_login_policy) return true;
    return src == AutoSource::pac && pac_is_trusted;
}

bool windows_login_allowed(ffy::proxy::Origin origin, bool discovered_login_policy) {
    using ffy::proxy::Origin;
    if (origin == Origin::configured || origin == Origin::pac) return true;
    if (origin == Origin::discovered) return discovered_login_policy;
    return false;  // kUntrustedOrigin (and anything unknown): never
}

namespace {

void wipe(std::string& s) {
    volatile char* p = s.data();
    for (size_t i = 0; i < s.size(); ++i) p[i] = 0;
    s.clear();
}

// For a proxy that must not get the Windows login: sends nothing, logs once per connection that would have needed it.
class NoLogin final : public ffy::proxy::Authenticator {
public:
    explicit NoLogin(ffy::proxy::Endpoint p) : proxy_(std::move(p)) {}
    std::optional<std::string> respond(const std::vector<std::string>& challenges) override {
        if (auto c = pick_sspi_challenge(challenges, "")) {
            why_ = proxy_.origin == kUntrustedOrigin
                       ? "the proxy requires the Windows login (" + c->scheme +
                             ") but comes from a policy file other users can change: not sending credentials; ask IT to restrict "
                             "the file to administrators or to set the proxy explicitly"
                       : "the proxy requires the Windows login (" + c->scheme +
                             ") but was discovered automatically (WPAD): not sending credentials; ask IT to set the proxy explicitly "
                             "(manual setting, netsh winhttp, PAC script or HTTPS_PROXY)";
            spdlog::warn("proxy: {}: {}", proxy_.str(), why_);
        }
        return std::nullopt;
    }
    std::string scheme() const override { return ""; }
    std::string declined() const override { return why_; }

private:
    ffy::proxy::Endpoint proxy_;
    std::string why_;
};

class StoredBasic final : public ffy::proxy::Authenticator {
public:
    StoredBasic(ffy::proxy::Endpoint p, std::function<std::optional<StoredLogin>()> read) : proxy_(std::move(p)), read_(std::move(read)) {}
    std::optional<std::string> respond(const std::vector<std::string>& challenges) override {
        if (sent_) return std::nullopt;  // a 407 after our credential: refused
        bool offered = std::any_of(challenges.begin(), challenges.end(), [](const std::string& c) {
            auto sp = c.find_first_of(" \t");
            return ffy::iequals(ffy::trim(c.substr(0, sp)), "Basic");
        });
        if (!offered) return std::nullopt;
        if (proxy_.origin != ffy::proxy::Origin::configured) {
            why_ = "Basic is sent only to a proxy set explicitly, not to one from a PAC script or automatic detection";
            return std::nullopt;
        }
        auto login = read_ ? read_() : std::nullopt;
        if (!login) return std::nullopt;
        std::string plain = login->user + ":" + login->password;
        wipe(login->password);
        wipe(login->user);
        std::string v = "Basic " + ffy::b64std_encode(plain);
        wipe(plain);
        sent_ = true;
        return v;
    }
    std::string scheme() const override { return sent_ ? "Basic" : ""; }
    std::string declined() const override { return why_; }

private:
    ffy::proxy::Endpoint proxy_;
    std::function<std::optional<StoredLogin>()> read_;
    std::string why_;
    bool sent_ = false;
};

}  // namespace

std::unique_ptr<ffy::proxy::Authenticator> login_authenticator(const ffy::proxy::Endpoint& proxy, bool discovered_login_policy,
                                                                const std::function<std::unique_ptr<ffy::proxy::Authenticator>()>& sspi) {
    if (windows_login_allowed(proxy.origin, discovered_login_policy)) return sspi ? sspi() : nullptr;
    return std::make_unique<NoLogin>(proxy);
}

std::unique_ptr<ffy::proxy::Authenticator> stored_basic(const ffy::proxy::Endpoint& proxy, std::function<std::optional<StoredLogin>()> read) {
    return std::make_unique<StoredBasic>(proxy, std::move(read));
}

std::optional<SspiChallenge> pick_sspi_challenge(const std::vector<std::string>& challenges, const std::string& chosen) {
    auto split = [](const std::string& c) {
        auto sp = c.find_first_of(" \t");
        return SspiChallenge{ffy::trim(c.substr(0, sp)), sp == std::string::npos ? std::string() : ffy::trim(c.substr(sp + 1))};
    };
    if (!chosen.empty()) {
        for (auto& c : challenges)
            if (auto s = split(c); ffy::iequals(s.scheme, chosen)) return SspiChallenge{chosen, s.token};
        return std::nullopt;
    }
    for (const char* want : {"Negotiate", "NTLM"})
        for (auto& c : challenges)
            if (auto s = split(c); ffy::iequals(s.scheme, want)) return SspiChallenge{want, s.token};
    return std::nullopt;
}

std::wstring proxy_spn(const std::string& proxy_host) {
    std::string h = proxy_host;
    if (h.size() > 1 && h.front() == '[' && h.back() == ']') h = h.substr(1, h.size() - 2);
    std::wstring w = L"HTTP/";
    for (unsigned char c : h) w.push_back(wchar_t(c));  // host names are ASCII (checked where they are parsed)
    return w;
}

#ifndef _WIN32
void install_system_proxy() {}
std::unique_ptr<ffy::proxy::Authenticator> make_sspi_authenticator(const std::string&) { return nullptr; }
#endif

}  // namespace fairyfly::link::sysproxy
