// Blocking HTTP/HTTPS client (Boost.Beast) with per-phase timeouts. Used for IdP introspection, JWKS, Google
// metadata/KMS/Firestore REST, the local fairyfly MCP endpoint and the OAuth token exchange.
#pragma once
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "util.hpp"

namespace ffy {

struct HttpRequest {
    std::string method = "GET";
    std::string url;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    int timeout_ms = 10000;          // applied to every I/O phase
    size_t max_body = 8u << 20;      // response body cap
    std::string connect_ip;          // optional: connect to exactly this checked address (SSRF guard), no proxy
};

struct HttpResponse {
    int status = 0;
    std::map<std::string, std::string> headers;  // lower-case names
    std::vector<std::string> set_cookies;        // every Set-Cookie header
    std::string body;
    std::string header(const std::string& lower_name) const {
        auto i = headers.find(lower_name);
        return i == headers.end() ? std::string() : i->second;
    }
};

// Throws ffy::Error on transport failure (DNS, connect, TLS, timeout, body too large). HTTP error statuses are returned.
HttpResponse http_do(const HttpRequest& req);
bool proxy_bypass(const std::string& host, const std::string& no_proxy);  // exposed for tests

}  // namespace ffy
