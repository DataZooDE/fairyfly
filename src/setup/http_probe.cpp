// The logic of the setup probes around one request; the transport (WinHTTP or a test's fake) is passed in.
#include <winsock2.h>
#include <windows.h>
#include <winhttp.h>

#include "include/setup/http_probe.h"

namespace fairyfly::setup {

namespace {

const char kInitialize[] =
    R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"fairyfly-setup-verify","version":"1"}}})";

}  // namespace

std::vector<std::pair<std::string, std::string>> probe_headers(const HttpProbeRequest& request) {
    std::vector<std::pair<std::string, std::string>> headers;
    const std::string connect_host = request.connect_host.empty() ? request.host : request.connect_host;
    if (connect_host != request.host) headers.emplace_back("Host", request.host + ":" + std::to_string(request.port));
    headers.insert(headers.end(), request.headers.begin(), request.headers.end());
    return headers;
}

HttpProbeResponse verify_round_trip(const VerifyRequest& req, const HttpTransport& transport) {
    HttpProbeRequest http;
    http.host = req.tls ? req.hostname : "127.0.0.1";
    http.port = req.port;
    http.tls = req.tls;
    http.method = "POST";
    http.path = "/mcp";
    http.headers = {{"Content-Type", "application/json"}, {"Accept", "application/json, text/event-stream"}};
    http.body = kInitialize;
    HttpProbeResponse r = transport(http);
    if (!r.ok && req.tls && (r.win_error == ERROR_WINHTTP_NAME_NOT_RESOLVED)) {
        http.connect_host = "127.0.0.1";   // pinned by thumbprint anyway; the Host header keeps the real name
        const HttpProbeResponse second = transport(http);
        if (second.ok) return second;
    }
    return r;
}

VerifyResult judge_verify(const HttpProbeResponse& r, const VerifyRequest& req) {
    VerifyResult out;
    if (!r.ok) {
        out.status = "failed";
        out.detail = "request failed: " + r.error;
        if (r.win_error == ERROR_WINHTTP_NAME_NOT_RESOLVED)
            out.detail += " (the host name does not resolve here: add a DNS record or hosts entry for " + req.hostname + ")";
        return out;
    }
    out.protocol = r.protocol;
    out.http_status = r.status;
    out.thumbprint_match = req.tls ? (!req.expected_thumbprint.empty() && r.cert_thumbprint == req.expected_thumbprint) : true;
    const bool auth_required = r.status == 401 && r.body.find("AUTH_REQUIRED") != std::string::npos;
    if (req.tls && !out.thumbprint_match) {
        out.status = "failed";
        out.detail = "the server presented a different certificate (" + r.cert_thumbprint + ")";
    } else if (!auth_required) {
        out.status = "failed";
        out.detail = "expected 401 AUTH_REQUIRED for an unauthenticated request, got HTTP " + std::to_string(r.status);
    } else if (r.www_authenticate.empty()) {
        out.status = "failed";
        out.detail = "401 without a WWW-Authenticate header";
    } else {
        out.status = "ok";
    }
    return out;
}

TlsProbe probe_tls(const std::string& host, int port, const std::string& expected, const HttpTransport& transport) {
    TlsProbe out;
    HttpProbeRequest req;
    req.host = host;
    req.port = port;
    req.tls = true;
    req.method = "GET";
    req.path = "/mcp";
    HttpProbeResponse r = transport(req);
    if (!r.ok && (r.win_error == ERROR_WINHTTP_NAME_NOT_RESOLVED || r.win_error == ERROR_WINHTTP_CANNOT_CONNECT || r.win_error == ERROR_WINHTTP_TIMEOUT) && host != "127.0.0.1") {
        req.connect_host = "127.0.0.1";   // the name may not resolve here; pin by thumbprint anyway
        r = transport(req);
    }
    if (!r.ok) {
        out.status = (r.win_error == ERROR_WINHTTP_SECURE_FAILURE || r.win_error == ERROR_WINHTTP_SECURE_CHANNEL_ERROR) ? "handshake_failed"
                     : (r.win_error == ERROR_WINHTTP_CANNOT_CONNECT || r.win_error == ERROR_WINHTTP_TIMEOUT || r.win_error == ERROR_WINHTTP_NAME_NOT_RESOLVED) ? "unreachable"
                                                                                                                                                               : "error";
        out.detail = r.error;
        return out;
    }
    out.status = "ok";
    out.protocol = r.protocol;
    out.thumbprint = r.cert_thumbprint;
    out.thumbprint_match = !expected.empty() && r.cert_thumbprint == expected;
    out.http_status = r.status;
    return out;
}

} // namespace fairyfly::setup
