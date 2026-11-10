// Outbound HTTP proxies for HTTPS requests and the link's WebSocket: which proxy a URL goes through (a resolver the
// application installs, e.g. fairyfly's Windows/WinHTTP one, with HTTPS_PROXY / NO_PROXY as an explicit override), and
// the CONNECT tunnel through it, authenticated with a pluggable scheme (Basic here; Negotiate/NTLM via SSPI on Windows,
// supplied by the application). TLS and the WebSocket upgrade then run inside the tunnel, end to end with the gateway:
// the proxy sees only host:port.
//
// Credentials are never logged, never part of an error message and never put into a URL.
#pragma once
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace ffy::proxy {

// An HTTP proxy (CONNECT). Proxies spoken to over TLS ("https://proxy") are not supported.
struct Endpoint {
    std::string host;
    int port = 0;
    std::string str() const;  // host:port, for messages
    bool operator==(const Endpoint&) const = default;
};

// One way to reach a URL: through a proxy, or DIRECT. A resolver returns them in the order to try; the next one is
// tried only when a proxy cannot be reached (TCP connect fails). A proxy that answers (403, 407, ...) is final.
// An empty list means DIRECT.
struct Route {
    bool direct = true;
    Endpoint proxy;
    static Route via(Endpoint e) { return Route{false, std::move(e)}; }
    bool operator==(const Route&) const = default;
};
using Routes = std::vector<Route>;

// Proxy authentication for one CONNECT attempt (a new instance per TCP connection to the proxy, since Negotiate and
// NTLM authenticate the connection, not the request).
class Authenticator {
public:
    virtual ~Authenticator() = default;
    // The proxy answered 407 with these Proxy-Authenticate values ("Negotiate", "NTLM <base64>", "Basic realm=..."),
    // the first time or as the next step of a handshake this authenticator started. Returns the Proxy-Authorization
    // value to send next on the same connection ("NTLM <base64>"), or nullopt when it has nothing (more) to offer.
    virtual std::optional<std::string> respond(const std::vector<std::string>& challenges) = 0;
    // The scheme in use, for messages ("Negotiate", "NTLM", "Basic"); "" before respond() picked one.
    virtual std::string scheme() const = 0;
};
using AuthenticatorFactory = std::function<std::unique_ptr<Authenticator>(const Endpoint& proxy)>;

// Basic, only with credentials configured explicitly (HTTPS_PROXY's user:password, or the application's own store).
std::unique_ptr<Authenticator> basic(std::string user, std::string password);

struct Config {
    // Routes for a URL. ws:// and wss:// URLs are passed as http:// and https://. Unset: DIRECT.
    std::function<Routes(const std::string& url)> resolve;
    // Authenticators to offer a proxy, tried in this order: the first that answers a 407 is used for that connection.
    std::vector<AuthenticatorFactory> authenticators;
    std::string source;  // where the routes come from, for messages: "windows", ...
};

// Installs the process-wide proxy configuration (at startup, before connections are made). The default has no
// resolver: only the environment decides.
void configure(Config c);

// HTTPS_PROXY / https_proxy (also for wss://) and NO_PROXY / no_proxy, parsed. They override the configured resolver:
// a host matching NO_PROXY goes DIRECT, else HTTPS_PROXY is used when set. Loopback always goes DIRECT.
struct EnvProxy {
    std::optional<Endpoint> proxy;
    std::string user, password;  // from user:password@ (percent-decoded); used only for Basic
    std::string no_proxy;
    std::string error;           // a value that could not be used, described without credentials
};
EnvProxy env_proxy();
// Parses a proxy URL ("http://[user:pass@]host[:port][/]", "host:port"); throws ffy::Error without echoing it.
EnvProxy parse_proxy_url(const std::string& value);

// The decision for a https:// or wss:// URL: environment override, then the configured resolver, else DIRECT.
struct Decision {
    Routes routes;
    std::string source;  // "environment", the configured source, or "default"
    std::vector<AuthenticatorFactory> authenticators;
};
Decision decide(const std::string& url);

// A TCP connection to host:port of a https:// or wss:// URL along the decision's routes: direct, or a CONNECT tunnel
// through the proxy with authentication. Returns the socket (to be owned by the caller, e.g. cpp-httplib's
// SocketFactory). Throws ProxyError / ffy::Error with a message naming the proxy and what it answered.
// timeout_ms bounds each connect and each read of the proxy's answer.
#ifdef _WIN32
using socket_handle = unsigned long long;  // SOCKET
#else
using socket_handle = int;
#endif
socket_handle open_tunnel(const std::string& host, int port, const Decision& d, int timeout_ms);

}  // namespace ffy::proxy
