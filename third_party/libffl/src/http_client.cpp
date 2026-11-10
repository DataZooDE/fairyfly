// Blocking HTTP/HTTPS client on cpp-httplib. HTTPS verifies the certificate chain against the platform trust store
// (system roots on Linux/macOS, the Windows certificate store on Windows) and the host name - cpp-httplib's defaults,
// which are never switched off here.
#include "http_client.hpp"

#include <httplib.h>

#include <algorithm>

#include "proxy.hpp"

namespace ffy {

// NO_PROXY entries are exact hosts or domain suffixes (".example.com", "*.example.com"); loopback is never proxied.
bool proxy_bypass(const std::string& host, const std::string& no_proxy) {
    // loopback: localhost, ::1 and 127.0.0.0/8 as an address (not a name like "127.evil.example")
    if (iequals(host, "localhost") || host == "::1" || host == "[::1]" ||
        (host.rfind("127.", 0) == 0 && host.find_first_not_of("0123456789.") == std::string::npos))
        return true;
    for (auto& e : split_csv(no_proxy)) {
        std::string d = lower(e);
        if (d == "*") return true;
        if (d.rfind("*.", 0) == 0) d = d.substr(1);
        std::string h = lower(host);
        if (h == d || (d.size() > 1 && d[0] == '.' && h.size() > d.size() && h.compare(h.size() - d.size(), d.size(), d) == 0) ||
            (d[0] != '.' && h.size() > d.size() + 1 && h.compare(h.size() - d.size() - 1, d.size() + 1, "." + d) == 0))
            return true;
    }
    return false;
}

// Plain http:// through HTTP_PROXY (forwarded by the proxy, not tunnelled). https:// goes through proxy.hpp: the
// environment override or the installed resolver, as a CONNECT tunnel (see http_do).
static void apply_http_proxy(httplib::Client& cli, const Url& u) {
    std::string p = env_or("HTTP_PROXY", env_or("http_proxy"));
    if (p.empty() || proxy_bypass(u.host, env_or("NO_PROXY", env_or("no_proxy")))) return;
    auto e = proxy::parse_proxy_url(p);
    cli.set_proxy(e.proxy->host, e.proxy->port);
    if (!e.user.empty()) cli.set_proxy_basic_auth(e.user, e.password);
}

HttpResponse http_do(const HttpRequest& rq) {
    Url u = parse_url(rq.url);
    if (u.scheme != "http" && u.scheme != "https") throw Error("unsupported scheme " + u.scheme);
    httplib::Client cli(u.scheme + "://" + u.host + ":" + u.port);
    const auto t = std::chrono::milliseconds(rq.timeout_ms);
    cli.set_connection_timeout(t);
    cli.set_read_timeout(t);
    cli.set_write_timeout(t);
    cli.set_payload_max_length(rq.max_body);
    cli.set_follow_location(false);
    cli.set_keep_alive(false);
    cli.set_path_encode(false);  // targets are built (and percent-encoded) by the callers
    std::string tunnel_error;  // why the proxy tunnel failed, in place of cpp-httplib's generic "Proxy connection failed"
    if (!rq.connect_ip.empty()) cli.set_hostname_addr_map({{u.host, rq.connect_ip}});  // TLS still checks the name
    else if (!u.tls()) apply_http_proxy(cli, u);
    else {
        auto d = proxy::decide(rq.url);
        if (!std::all_of(d.routes.begin(), d.routes.end(), [](const proxy::Route& r) { return r.direct; })) {
            cli.set_socket_factory([d, host = u.host, port = std::stoi(u.port), t = rq.timeout_ms, &tunnel_error](httplib::Error& e) -> socket_t {
                try {
                    return proxy::open_tunnel(host, port, d, t);
                } catch (const std::exception& x) {
                    tunnel_error = x.what();
                    e = httplib::Error::ProxyConnection;
                    return INVALID_SOCKET;
                }
            });
        }
    }

    httplib::Headers headers{{"User-Agent", "fairyfly-gateway/0.2"}};
    std::string ctype;
    for (auto& [k, v] : rq.headers) {
        if (iequals(k, "Content-Type")) ctype = v;
        else headers.emplace(k, v);
    }
    if (ctype.empty()) ctype = "application/octet-stream";

    httplib::Result res;
    const std::string& m = rq.method;
    if (m == "GET") res = cli.Get(u.target, headers);
    else if (m == "HEAD") res = cli.Head(u.target, headers);
    else if (m == "POST") res = cli.Post(u.target, headers, rq.body, ctype);
    else if (m == "PATCH") res = cli.Patch(u.target, headers, rq.body, ctype);
    else if (m == "PUT") res = cli.Put(u.target, headers, rq.body, ctype);
    else if (m == "DELETE") res = cli.Delete(u.target, headers, rq.body, ctype);
    else throw Error("unsupported method " + m);
    if (!res) throw HttpTransportError("http " + m + " " + rq.url + ": " + (tunnel_error.empty() ? httplib::to_string(res.error()) : tunnel_error));

    HttpResponse out;
    out.status = res->status;
    for (auto& [k, v] : res->headers) {
        out.headers[lower(k)] = v;
        if (iequals(k, "Set-Cookie")) out.set_cookies.push_back(v);
    }
    out.body = std::move(res->body);
    return out;
}

}  // namespace ffy
