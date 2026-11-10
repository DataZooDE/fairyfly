// Outbound HTTP proxies: the decision (environment override, then the installed resolver) and the CONNECT tunnel with
// proxy authentication. See proxy.hpp.
#include "proxy.hpp"

#include <httplib.h>
#include <spdlog/spdlog.h>

#include <cctype>
#include <mutex>
#include <type_traits>

#include "http_client.hpp"
#include "util.hpp"

namespace ffy::proxy {

static_assert(std::is_same_v<socket_handle, socket_t>, "proxy::socket_handle must be cpp-httplib's socket_t");

std::string Endpoint::str() const {
    return (host.find(':') != std::string::npos && host[0] != '[' ? "[" + host + "]" : host) + ":" + std::to_string(port);
}

// ---- configuration ----------------------------------------------------------------------------------------------

namespace {
std::mutex cfg_mu;
Config& cfg() {
    static Config c;
    return c;
}

std::string scheme_of(const std::string& challenge) {
    auto sp = challenge.find_first_of(" \t");
    return trim(challenge.substr(0, sp));
}

class BasicAuth final : public Authenticator {
public:
    BasicAuth(std::string u, std::string p) : user_(std::move(u)), pass_(std::move(p)) {}
    std::optional<std::string> respond(const std::vector<std::string>& challenges) override {
        if (sent_) return std::nullopt;  // a 407 after our credentials: they were refused
        for (auto& c : challenges)
            if (iequals(scheme_of(c), "Basic")) {
                sent_ = true;
                return "Basic " + b64std_encode(user_ + ":" + pass_);
            }
        return std::nullopt;
    }
    std::string scheme() const override { return sent_ ? "Basic" : ""; }

private:
    std::string user_, pass_;
    bool sent_ = false;
};
}  // namespace

std::unique_ptr<Authenticator> basic(std::string user, std::string password) {
    return std::make_unique<BasicAuth>(std::move(user), std::move(password));
}

void configure(Config c) {
    std::lock_guard<std::mutex> l(cfg_mu);
    cfg() = std::move(c);
}

EnvProxy parse_proxy_url(const std::string& value) {
    EnvProxy e;
    std::string v = trim(value);
    auto sch = v.find("://");
    if (sch != std::string::npos) {
        std::string s = lower(v.substr(0, sch));
        if (s != "http") throw Error("proxy scheme " + s.substr(0, 16) + ":// is not supported (use http://host:port)");
        v = v.substr(sch + 3);
    }
    if (auto slash = v.find('/'); slash != std::string::npos) {
        if (v.find_first_not_of('/', slash) != std::string::npos) throw Error("a proxy address has no path (use http://host:port)");
        v = v.substr(0, slash);
    }
    if (auto at = v.rfind('@'); at != std::string::npos) {
        std::string ui = v.substr(0, at);
        v = v.substr(at + 1);
        auto colon = ui.find(':');
        e.user = url_decode(ui.substr(0, colon));
        if (colon != std::string::npos) e.password = url_decode(ui.substr(colon + 1));
    }
    Endpoint ep;
    std::string port;
    if (!v.empty() && v[0] == '[') {
        auto close = v.find(']');
        if (close == std::string::npos) throw Error("proxy address: bad IPv6 host");
        ep.host = v.substr(0, close + 1);
        if (close + 1 < v.size()) {
            if (v[close + 1] != ':') throw Error("proxy address: bad host");
            port = v.substr(close + 2);
        }
    } else {
        auto colon = v.find(':');
        ep.host = v.substr(0, colon);
        if (colon != std::string::npos) port = v.substr(colon + 1);
    }
    for (unsigned char c : ep.host)
        if (!(std::isalnum(c) || c == '.' || c == '-' || c == '_' || c == ':' || c == '[' || c == ']')) throw Error("proxy address: bad host name");
    if (ep.host.empty() || ep.host == "[]") throw Error("proxy address: no host");
    if (port.empty()) ep.port = 80;
    else {
        if (port.size() > 5 || port.find_first_not_of("0123456789") != std::string::npos) throw Error("proxy address: bad port");
        ep.port = std::stoi(port);
        if (ep.port < 1 || ep.port > 65535) throw Error("proxy address: bad port");
    }
    e.proxy = ep;
    return e;
}

EnvProxy env_proxy() {
    EnvProxy e;
    std::string v = env_or("HTTPS_PROXY", env_or("https_proxy"));
    if (!trim(v).empty()) {
        try {
            e = parse_proxy_url(v);
        } catch (const std::exception& x) {
            e = EnvProxy{};
            e.error = std::string("HTTPS_PROXY: ") + x.what();  // never the value: it may hold a password
        }
    }
    e.no_proxy = env_or("NO_PROXY", env_or("no_proxy"));
    return e;
}

static std::string bare_host(const std::string& h) {
    return h.size() > 1 && h.front() == '[' && h.back() == ']' ? h.substr(1, h.size() - 2) : h;
}

Decision decide(const std::string& url) {
    Url u = parse_url(url);
    Decision d;
    EnvProxy env = env_proxy();
    if (proxy_bypass(u.host, env.no_proxy) || proxy_bypass(bare_host(u.host), env.no_proxy)) {  // also: loopback, always
        d.source = "environment (NO_PROXY) or loopback";
        return d;
    }
    Config c;
    {
        std::lock_guard<std::mutex> l(cfg_mu);
        c = cfg();
    }
    if (!env.error.empty()) throw Error(env.error);
    if (env.proxy) {
        d.source = "environment (HTTPS_PROXY)";
        d.routes.push_back(Route::via(*env.proxy));
        d.authenticators = c.authenticators;
        if (!env.user.empty() || !env.password.empty())
            d.authenticators.push_back([user = env.user, pw = env.password](const Endpoint&) { return basic(user, pw); });
        return d;
    }
    if (c.resolve) {
        std::string q = url;
        if (u.scheme == "wss") q = "https" + url.substr(3);
        else if (u.scheme == "ws") q = "http" + url.substr(2);
        d.routes = c.resolve(q);
        d.source = c.source.empty() ? "resolver" : c.source;
        d.authenticators = c.authenticators;
        return d;
    }
    d.source = "default";
    return d;
}

// ---- the tunnel -------------------------------------------------------------------------------------------------

namespace {


struct Closer {
    socket_t s;
    bool keep = false;
    ~Closer() {
        if (!keep && s != INVALID_SOCKET) httplib::detail::close_socket(s);
    }
};

socket_t tcp_connect(const std::string& host, int port, int timeout_ms, std::string& why) {
    auto err = httplib::Error::Success;
    time_t sec = timeout_ms / 1000, usec = (timeout_ms % 1000) * 1000;
    socket_t s = httplib::detail::create_client_socket(bare_host(host), std::string(), port, AF_UNSPEC, CPPHTTPLIB_TCP_NODELAY, false,
                                                       nullptr, sec, usec, sec, usec, sec, usec, std::string(), err);
    if (s == INVALID_SOCKET) why = httplib::to_string(err == httplib::Error::Success ? httplib::Error::Connection : err);
    return s;
}

bool send_all(socket_t s, const std::string& data) {
#ifdef MSG_NOSIGNAL
    const int flags = MSG_NOSIGNAL;
#else
    const int flags = 0;
#endif
    size_t off = 0;
    while (off < data.size()) {
        auto n = httplib::detail::send_socket(s, data.data() + off, data.size() - off, flags);
        if (n <= 0) return false;
        off += size_t(n);
    }
    return true;
}

struct ProxyResponse {
    int status = 0;
    std::string reason;
    std::vector<std::pair<std::string, std::string>> headers;  // lower-case names
    bool reusable = false;  // the proxy keeps this connection open for the next request
    std::vector<std::string> all(const std::string& name) const {
        std::vector<std::string> v;
        for (auto& [k, x] : headers)
            if (k == name) v.push_back(x);
        return v;
    }
    std::string get(const std::string& name) const {
        for (auto& [k, x] : headers)
            if (k == name) return x;
        return {};
    }
};

// Reads exactly the proxy's answer (byte by byte up to the blank line, then a declared body), never into the tunnel.
bool read_exact(socket_t s, char* p, size_t n) {
    while (n) {
        auto r = httplib::detail::read_socket(s, p, n, 0);
        if (r <= 0) return false;
        p += r;
        n -= size_t(r);
    }
    return true;
}

bool read_line(socket_t s, std::string& line, size_t& budget) {
    line.clear();
    char c;
    while (budget) {
        if (!read_exact(s, &c, 1)) return false;
        --budget;
        if (c == '\n') {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            return true;
        }
        line.push_back(c);
    }
    return false;
}

std::optional<ProxyResponse> read_response(socket_t s) {
    ProxyResponse r;
    size_t budget = 64 * 1024;
    std::string line;
    if (!read_line(s, line, budget)) return std::nullopt;
    // HTTP/1.1 407 Proxy Authentication Required
    if (line.rfind("HTTP/1.", 0) != 0 || line.size() < 12) return std::nullopt;
    bool http11 = line[7] == '1';
    try {
        r.status = std::stoi(line.substr(9, 3));
    } catch (...) {
        return std::nullopt;
    }
    r.reason = line.size() > 13 ? trim(line.substr(13)) : "";
    for (;;) {
        if (!read_line(s, line, budget)) return std::nullopt;
        if (line.empty()) break;
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        r.headers.emplace_back(lower(trim(line.substr(0, colon))), trim(line.substr(colon + 1)));
    }
    std::string conn = lower(r.get("connection") + "," + r.get("proxy-connection"));
    bool keep = http11 ? conn.find("close") == std::string::npos : conn.find("keep-alive") != std::string::npos;
    if (r.status / 100 == 2) {
        r.reusable = true;  // the tunnel: no body
        return r;
    }
    // drain the body so that the next request on this connection starts at a message boundary
    std::string cl = r.get("content-length");
    bool known = false;
    if (!cl.empty()) {
        try {
            long long n = std::stoll(cl);
            if (n >= 0 && n <= (1 << 20)) {
                std::string body(size_t(n), '\0');
                known = n == 0 || read_exact(s, body.data(), body.size());
            }
        } catch (...) {
        }
    } else if (lower(r.get("transfer-encoding")).find("chunked") != std::string::npos) {
        size_t total = 0;
        for (;;) {
            size_t b = 1024;
            if (!read_line(s, line, b)) break;
            size_t n = 0;
            try {
                n = std::stoul(line, nullptr, 16);
            } catch (...) {
                break;
            }
            total += n;
            if (total > (1u << 20)) break;
            std::string chunk(n + 2, '\0');
            if (!read_exact(s, chunk.data(), chunk.size())) break;
            if (n == 0) {
                known = true;  // (trailers are not expected after a proxy's error page)
                break;
            }
        }
    } else if (r.status == 304 || r.status == 204) {
        known = true;
    }
    r.reusable = keep && known;
    return r;
}

std::string offered(const std::vector<std::string>& challenges) {
    std::string out;
    for (auto& c : challenges) {
        std::string s = scheme_of(c).substr(0, 32);
        if (s.empty() || out.find(s) != std::string::npos) continue;
        out += (out.empty() ? "" : ", ") + s;
    }
    return out.empty() ? "no scheme" : out;
}

struct Refused : Error {
    using Error::Error;
};

// One proxy: connect, CONNECT, authenticate. Returns the tunnel; throws Refused when the proxy answered (final), or
// Error when it could not be reached (the next route may be tried).
socket_t through(const Endpoint& px, const std::string& host, int port, const Decision& d, int timeout_ms) {
    std::string why;
    socket_t s = tcp_connect(px.host, px.port, timeout_ms, why);
    if (s == INVALID_SOCKET) throw Error("cannot reach proxy " + px.str() + " (" + why + ")");
    Closer guard{s};
    const std::string target = (host.find(':') != std::string::npos && host[0] != '[' ? "[" + host + "]" : host) + ":" + std::to_string(port);
    const std::string via = " (" + px.str() + " -> " + target + ", proxy from " + d.source + ")";

    std::vector<std::unique_ptr<Authenticator>> auths;
    for (auto& f : d.authenticators)
        if (f)
            if (auto a = f(px)) auths.push_back(std::move(a));
    Authenticator* active = nullptr;
    std::optional<std::string> authz;
    for (int round = 0; round < 8; ++round) {
        std::string req = "CONNECT " + target + " HTTP/1.1\r\nHost: " + target + "\r\nUser-Agent: ffy-link/0.2\r\nProxy-Connection: Keep-Alive\r\n";
        if (authz) req += "Proxy-Authorization: " + *authz + "\r\n";
        req += "\r\n";
        if (!send_all(guard.s, req)) throw Refused("proxy closed the connection" + via);
        auto res = read_response(guard.s);
        if (!res) throw Refused("proxy sent no valid answer to CONNECT" + via);
        if (res->status / 100 == 2) {
            spdlog::debug("proxy: tunnel to {} through {} ({}{})", target, px.str(), d.source,
                          active && !active->scheme().empty() ? ", authenticated with " + active->scheme() : "");
            guard.keep = true;
            return guard.s;
        }
        if (res->status != 407) {
            throw Refused("proxy refused CONNECT " + std::to_string(res->status) + (res->reason.empty() ? "" : " " + res->reason.substr(0, 80)) + via);
        }
        auto ch = res->all("proxy-authenticate");
        std::optional<std::string> next;
        if (active) {
            next = active->respond(ch);
            if (!next)
                throw Refused("proxy requires authentication: it refused the " + (active->scheme().empty() ? std::string("offered") : active->scheme()) +
                              " credentials of this user (offers " + offered(ch) + ")" + via);
        } else {
            for (auto& a : auths)
                if ((next = a->respond(ch))) {
                    active = a.get();
                    break;
                }
            if (!next)
                throw Refused("proxy requires authentication (offers " + offered(ch) + "), and no credentials are configured for it" +
                              (auths.empty() ? std::string() : " for these schemes") + via);
        }
        if (!res->reusable) {
            // the proxy ends the connection after its 407: the first answer only told us the schemes, so start over on a new
            // connection; in the middle of a connection-oriented handshake (NTLM's challenge) there is no way to go on
            if (authz) throw Refused("proxy closed the connection during " + active->scheme() + " authentication" + via);
            httplib::detail::close_socket(guard.s);
            guard.s = tcp_connect(px.host, px.port, timeout_ms, why);
            if (guard.s == INVALID_SOCKET) throw Error("cannot reach proxy " + px.str() + " (" + why + ")");
        }
        authz = std::move(next);
    }
    throw Refused("proxy authentication did not complete" + via);
}

}  // namespace

socket_handle open_tunnel(const std::string& host, int port, const Decision& d, int timeout_ms) {
    Routes routes = d.routes.empty() ? Routes{Route{}} : d.routes;
    std::string errors;
    for (auto& r : routes) {
        if (r.direct) {
            std::string why;
            socket_t s = tcp_connect(host, port, timeout_ms, why);
            if (s != INVALID_SOCKET) return s;
            errors += (errors.empty() ? "" : "; ") + std::string("direct: ") + why;
            continue;
        }
        try {
            return through(r.proxy, host, port, d, timeout_ms);
        } catch (const Refused&) {
            throw;
        } catch (const std::exception& e) {
            spdlog::warn("proxy: {}", e.what());
            errors += (errors.empty() ? "" : "; ") + std::string(e.what());
        }
    }
    throw Error(errors.empty() ? "no route" : errors);
}

}  // namespace ffy::proxy
