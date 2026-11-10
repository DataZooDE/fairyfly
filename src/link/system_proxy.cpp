// The portable part of the Windows proxy settings (see include/link/system_proxy.h): proxy lists, bypass lists, the
// order of the settings and the choice of an SSPI challenge. The WinHTTP and SSPI calls are in system_proxy_win32.cpp.
#include "include/link/system_proxy.h"

#include <algorithm>
#include <cctype>

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
    if (l.rfind("http://", 0) == 0) v = v.substr(7);
    else if (l.find("://") != std::string::npos) return std::nullopt;  // https:// (TLS to the proxy), socks://: not supported
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
        if (p.front() == '.') p = "*" + p;
        if (glob(p, host) || glob(p, hostport)) return true;
    }
    return false;
}

Resolution resolve(const ProxySettings& user, const std::optional<ProxySettings>& machine, const std::string& url,
                   const std::function<AutoProxyResult()>& autoproxy) {
    ffy::Url u = ffy::parse_url(url);
    const int port = std::stoi(u.port);
    std::string note;
    if (user.auto_detect || !user.auto_config_url.empty()) {
        const std::string what = !user.auto_config_url.empty() ? "PAC " + without_query(user.auto_config_url)
                                                                : std::string("automatic detection (WPAD)");
        AutoProxyResult r = autoproxy ? autoproxy() : AutoProxyResult{false, false, {}, {}, "not available"};
        if (r.ok) {
            if (r.direct) return {{}, what + ": DIRECT"};
            if (bypassed(u.host, port, r.bypass)) return {{}, what + ": bypass list"};
            auto routes = parse_proxy_list(r.proxy);
            return {routes, routes.empty() ? what + ": DIRECT" : what};
        }
        note = what + " failed (" + r.error + "), ";
    }
    if (!user.proxy.empty()) {
        if (bypassed(u.host, port, user.bypass)) return {{}, note + "manual proxy settings: bypass list"};
        auto routes = parse_proxy_list(user.proxy);
        return {routes, note + (routes.empty() ? "manual proxy settings: none for https" : "manual proxy settings")};
    }
    if (machine && !machine->proxy.empty()) {
        if (bypassed(u.host, port, machine->bypass)) return {{}, note + "WinHTTP machine setting: bypass list"};
        auto routes = parse_proxy_list(machine->proxy);
        return {routes, note + "WinHTTP machine setting (netsh winhttp)"};
    }
    return {{}, note + "no proxy configured"};
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
