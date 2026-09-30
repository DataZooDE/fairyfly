// Real VerifyHost: proves the listener end to end. When the prefix is already served (tray or server running) the
// round trip goes to that server; otherwise a deny-all McpHttpServer is bound on the real prefix for the duration of
// one request. Expected answer: 401 AUTH_REQUIRED with WWW-Authenticate, over TLS whose certificate is pinned by
// thumbprint.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#include <chrono>
#include <memory>
#include <thread>

#include "include/mcp/authenticators.h"
#include "include/mcp/http_server.h"
#include "include/setup/setup_hosts.h"
#include "winhttp_client.h"

namespace fairyfly::setup {

namespace {

using namespace fairyfly::mcp;

/// No tools: every request is refused by the deny-all authenticator before it reaches a provider.
class NoTools : public ToolProvider {
public:
    std::vector<ToolDef> list_tools() const override { return {}; }
    bool has_tool(const std::string&) const override { return false; }
    ToolResult call_tool(const std::string&, const json&, const CallContext&) override {
        ToolResult r;
        r.is_error = true;
        return r;
    }
};

const char kInitialize[] =
    R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"fairyfly-setup-verify","version":"1"}}})";

VerifyResult judge(const HttpProbeResponse& r, const VerifyRequest& req) {
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

HttpProbeResponse do_request(const VerifyRequest& req) {
    HttpProbeRequest http;
    http.host = req.tls ? req.hostname : "127.0.0.1";
    http.port = req.port;
    http.tls = req.tls;
    http.method = "POST";
    http.path = "/mcp";
    http.headers = {{"Content-Type", "application/json"}, {"Accept", "application/json, text/event-stream"}};
    http.body = kInitialize;
    HttpProbeResponse r = winhttp_request(http);
    if (!r.ok && req.tls && (r.win_error == ERROR_WINHTTP_NAME_NOT_RESOLVED)) {
        http.connect_host = "127.0.0.1";   // pinned by thumbprint anyway; the Host header keeps the real name
        const HttpProbeResponse second = winhttp_request(http);
        if (second.ok) return second;
    }
    return r;
}

class RealVerifyHost : public VerifyHost {
public:
    VerifyResult round_trip(const VerifyRequest& req) override {
        auto probe = make_real_system_probe();
        if (probe->tcp_listening("127.0.0.1", req.port)) {
            // Something already serves the prefix (tray running): verify against it.
            VerifyResult r = judge(do_request(req), req);
            if (r.status == "failed") r.detail += " (verified against the already running server)";
            return r;
        }
        if (!req.owner_sid.empty() && make_real_elevator()->current_user_sid() != req.owner_sid) {
            VerifyResult r;
            r.status = "skipped";
            r.detail = "the reservation belongs to another account; start the server as that user to verify";
            return r;
        }
        HttpServerConfig cfg;
        cfg.host = req.tls ? "+" : "127.0.0.1";
        cfg.port = req.port;
        cfg.tls = req.tls;
        cfg.read_only = true;
        cfg.worker_threads = 2;
        cfg.endpoint.allowed_hosts = {req.hostname};
        NoTools provider;
        McpHttpServer server(cfg, provider, std::make_unique<DenyAllAuthenticator>());
        std::string error;
        if (!server.bind(&error)) {
            VerifyResult r;
            r.status = "failed";
            r.detail = "cannot bind the listener: " + error;
            return r;
        }
        std::thread runner([&] { server.run(); });
        for (int i = 0; i < 40 && !probe->tcp_listening("127.0.0.1", req.port); ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
        VerifyResult r = judge(do_request(req), req);
        server.request_stop();
        runner.join();
        return r;
    }
};

} // namespace

std::unique_ptr<VerifyHost> make_real_verify_host() { return std::make_unique<RealVerifyHost>(); }

} // namespace fairyfly::setup
