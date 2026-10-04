#pragma once
// HTTP transport of `fairyfly mcp --http`: a thin http.sys (HTTP Server API) adapter around HttpEndpoint, the
// CallExecutor main-thread loop, IServerControl and the posture banner. TLS is terminated in-kernel by http.sys
// (certificate bound by `fairyfly mcp setup`); plain HTTP is for loopback development. Never writes to stdout.

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "include/mcp/call_executor.h"
#include "include/mcp/http_endpoint.h"
#include "include/mcp/principal.h"
#include "include/mcp/types.h"

namespace fairyfly::mcp {

/// ToolProvider that forwards to a replaceable delegate. Main thread only (like every provider);
/// used to rebuild the dispatcher when the read-only mode is toggled at runtime.
class ReloadableProvider : public ToolProvider {
public:
    explicit ReloadableProvider(std::unique_ptr<ToolProvider> delegate) : delegate_(std::move(delegate)) {}
    void reset(std::unique_ptr<ToolProvider> delegate) { delegate_ = std::move(delegate); }
    std::vector<ToolDef> list_tools() const override { return delegate_->list_tools(); }
    std::vector<ToolDef> list_tools_for(const Principal& principal) const override { return delegate_->list_tools_for(principal); }
    bool has_tool(const std::string& name) const override { return delegate_->has_tool(name); }
    ToolResult call_tool(const std::string& name, const json& args, const CallContext& ctx) override {
        return delegate_->call_tool(name, args, ctx);
    }
    void set_client_info(const json& client_info) override { delegate_->set_client_info(client_info); }
private:
    std::unique_ptr<ToolProvider> delegate_;
};

struct HttpServerConfig {
    std::string host = "127.0.0.1";
    int port = 8383;                       ///< fixed port; 0 is an error (http.sys has no ephemeral ports)
    HttpEndpointOptions endpoint;
    bool insecure_no_auth = false;         ///< for the posture banner only (the authenticator decides)
    bool read_only = true;                 ///< initial mode
    bool read_only_cap = false;            ///< FAIRYFLY_READ_ONLY: write mode can never be enabled
    std::size_t max_queue = 16;
    int call_timeout_ms = 120000;
    int worker_threads = 16;
    bool tls = false;                      ///< HTTPS binding (http.sys terminates TLS)
    std::vector<std::string> allow_ip;     ///< client allow-list (addresses/CIDR); empty = all
    bool insecure_http = false;            ///< allow plain HTTP on a non-loopback host (flag only)
};

class McpHttpServer : public IServerControl {
public:
    /// `apply_read_only` runs on the main thread (inside the executor loop) when set_read_only()
    /// changes the mode; it must rebuild/adjust the provider so SUBSEQUENT calls use the new mode.
    McpHttpServer(HttpServerConfig config, ToolProvider& provider, std::unique_ptr<IAuthenticator> authenticator,
                  std::function<void(bool read_only)> apply_read_only = {});
    ~McpHttpServer() override;

    /// Registers the http.sys prefix (server session, URL group, request queue). Idempotent. Returns false with a
    /// message on failure; bind_error_reason() then holds one of: no_url_reservation, prefix_registered,
    /// invalid_prefix, insecure_bind, http_sys_error.
    bool bind(std::string* error = nullptr);
    const std::string& bind_error_reason() const;
    /// The registered http.sys prefix ("https://+:8443/mcp/"); empty before a successful bind().
    const std::string& prefix() const;
    int port() const;
    /// Runs the main-thread executor loop (the CALLING thread must be the COM/main thread) and the
    /// http.sys receive workers on helper threads. Returns immediately when a stop was requested before. Returns 0 after request_stop()/request_restart().
    int run();
    bool restart_requested() const { return restart_.load(); }

    HttpEndpoint& endpoint();
    CallExecutor& executor();
    /// Owns the routed lanes through shutdown; configure before run().
    void set_session_router(std::shared_ptr<SessionExecutorPool> pool, HttpEndpoint::SessionRouter router,
                            HttpEndpoint::SessionRouter recheck_router = {});
    /// Interrupt blocked private worker calls before routed lanes are joined.
    /// Configure before bind; invoked once on shutdown, including destruction.
    void set_session_shutdown(std::function<void()> shutdown);
    /// Apply mode changes to routed lanes synchronously, before set_read_only
    /// returns; the global handler is still updated on its COM executor.
    void set_session_mode_hook(std::function<void(bool)> hook);
    /// Multi-line startup banner (also written to stderr by run_mcp_http).
    std::vector<std::string> posture_lines() const;

    // IServerControl
    ServerStatus status() const override;
    void set_read_only(bool read_only) override;
    void request_stop() override;
    void request_restart() override;

private:
    struct Impl;
    std::mutex routing_mu_;
    bool routing_locked_ = false;
    std::unique_ptr<Impl> impl_;
    HttpServerConfig config_;
    std::atomic<bool> read_only_;
    std::atomic<bool> stop_{false};
    std::atomic<bool> restart_{false};
    std::atomic<bool> running_{false};
    std::atomic<bool> session_routed_{false};
};

/// Scheme-aware endpoint URL for banners/status: https://<first allowed host or +>:<port><path> when `tls`,
/// http://127.0.0.1:<port><path> for plain loopback ("localhost" is shown as 127.0.0.1).
std::string http_endpoint_url(bool tls, const std::string& host, const std::vector<std::string>& allowed_hosts, int port,
                              const std::string& path);

/// Everything run_mcp hands over to the HTTP transport.
struct HttpRunArgs {
    ServeOptions options;
    ServerOptions server_options;
    bool read_only = true;
    bool read_only_cap = false;
    /// Builds the dispatcher for a mode (called on the main thread, initially and on every toggle).
    std::function<std::unique_ptr<ToolProvider>(bool read_only)> make_provider;
    /// Main thread: propagate the mode to the CLI handler (handler.set_read_only).
    std::function<void(bool read_only)> apply_read_only;
    /// Configure session routing while the server is still unbound. The
    /// callback may install a session pool and router with set_session_router.
    std::function<void(McpHttpServer&)> configure_session_routing;
    /// Phase 4 (tray) hook: receives the running server's IServerControl before the loop starts.
    std::function<void(IServerControl&)> on_control;
    /// Overrides the authenticator (tests); default make_http_authenticator(options.insecure_no_auth).
    std::unique_ptr<IAuthenticator> authenticator;
    /// Banner sink; default: std::cerr.
    std::function<void(const std::string& line)> log_line;
};

/// Runs `fairyfly mcp --http` until stop. Returns the process exit code (2 = could not bind).
/// `restart_requested` (optional) is set when IServerControl::request_restart() ended the run.
int run_mcp_http(HttpRunArgs args, bool* restart_requested = nullptr);

} // namespace fairyfly::mcp
