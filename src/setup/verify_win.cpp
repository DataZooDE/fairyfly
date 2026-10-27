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
#include "include/setup/http_probe.h"

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

class RealVerifyHost : public VerifyHost {
public:
    VerifyResult round_trip(const VerifyRequest& req) override {
        auto probe = make_real_system_probe();
        if (probe->tcp_listening("127.0.0.1", req.port)) {
            // Something already serves the prefix (tray running): verify against it.
            VerifyResult r = judge_verify(verify_round_trip(req, winhttp_request), req);
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
        VerifyResult r = judge_verify(verify_round_trip(req, winhttp_request), req);
        server.request_stop();
        runner.join();
        return r;
    }
};

} // namespace

std::unique_ptr<VerifyHost> make_real_verify_host() { return std::make_unique<RealVerifyHost>(); }

} // namespace fairyfly::setup
