// cpp-httplib must come first (it includes winsock2.h before windows.h).
#include <httplib.h>

#include "include/mcp/http_server.h"

#include <algorithm>
#include <chrono>
#include <iostream>
#include <thread>

#include <spdlog/spdlog.h>

#include "include/core.h"
#include "include/mcp/authenticators.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace fairyfly::mcp {

namespace {

bool is_loopback_bind(const std::string& host) {
    return host == "127.0.0.1" || host == "::1" || host == "localhost" || host == "[::1]";
}

HttpRequest to_request(const httplib::Request& req) {
    HttpRequest out;
    out.method = req.method;
    out.path = req.path;
    for (const auto& h : req.headers) {
        auto it = out.headers.find(h.first);
        if (it == out.headers.end()) out.headers.emplace(h.first, h.second);
        else it->second += ", " + h.second;
    }
    out.body = req.body;
    out.peer_addr = req.remote_addr;
    return out;
}

/// SseSink over a cpp-httplib DataSink.
class HttplibSink : public SseSink {
public:
    HttplibSink(httplib::DataSink& sink, const httplib::Request& req) : sink_(sink), req_(req) {}
    bool write(std::string_view frame) override { return sink_.write(frame.data(), frame.size()); }
    bool connected() const override { return !req_.is_connection_closed(); }
private:
    httplib::DataSink& sink_;
    const httplib::Request& req_;
};

void write_response(const httplib::Request& req, httplib::Response& res, HttpResponse r) {
    res.status = r.status;
    for (const auto& h : r.headers) res.set_header(h.first, h.second);
    if (r.stream) {
        auto stream = r.stream;
        const httplib::Request* request = &req;
        res.set_chunked_content_provider("text/event-stream", [stream, request](size_t, httplib::DataSink& sink) {
            HttplibSink adapter(sink, *request);
            try {
                stream(adapter);
            } catch (const std::exception& e) {
                spdlog::error("SSE stream failed: {}", e.what());
            }
            sink.done();
            return true;
        });
    } else if (!r.body.empty()) {
        res.set_content(r.body, r.header("Content-Type").empty() ? "application/json" : r.header("Content-Type"));
    }
}

} // namespace

struct McpHttpServer::Impl {
    Impl(const HttpServerConfig& cfg, ToolProvider& provider, std::unique_ptr<IAuthenticator> auth_in,
         std::function<void(bool)> apply)
        : auth(auth_in ? std::move(auth_in) : std::make_unique<DenyAllAuthenticator>()),
          executor(cfg.max_queue, cfg.call_timeout_ms),
          endpoint(cfg.endpoint, executor, provider, auth.get()),
          apply_read_only(std::move(apply)) {}

    std::unique_ptr<IAuthenticator> auth;
    CallExecutor executor;
    HttpEndpoint endpoint;
    std::function<void(bool)> apply_read_only;
    httplib::Server svr;
    int bound_port = 0;
    bool bound = false;
};

McpHttpServer::McpHttpServer(HttpServerConfig config, ToolProvider& provider,
                             std::unique_ptr<IAuthenticator> authenticator,
                             std::function<void(bool)> apply_read_only)
    : impl_(std::make_unique<Impl>(config, provider, std::move(authenticator), std::move(apply_read_only))),
      config_(std::move(config)), read_only_(config_.read_only) {
    Impl& impl = *impl_;
    const std::string path = config_.endpoint.path;

    const int workers = std::max(2, config_.worker_threads);
    impl.svr.new_task_queue = [workers] { return new httplib::ThreadPool(static_cast<size_t>(workers)); };
    impl.svr.set_payload_max_length(config_.endpoint.max_body_bytes);
    impl.svr.set_read_timeout(30, 0);
    impl.svr.set_write_timeout(30, 0);
    impl.svr.set_keep_alive_max_count(100);

    // Header-only checks (path, method, Host, Origin, Content-Type, size) before any body is read.
    impl.svr.set_pre_routing_handler([&impl](const httplib::Request& req, httplib::Response& res) {
        if (auto rejected = impl.endpoint.precheck(to_request(req))) {
            // Answering before the body was read makes the OS reset the connection and the client may
            // lose the response. So only body-less requests and over-limit bodies are refused here;
            // any other rejection is produced by the POST handler (which re-runs precheck) after the
            // body was read, still before authentication and JSON parsing.
            const std::string length = req.get_header_value("Content-Length");
            const bool body_pending = (!length.empty() && length != "0") || req.has_header("Transfer-Encoding");
            if (body_pending && rejected->status != 413) return httplib::Server::HandlerResponse::Unhandled;
            write_response(req, res, std::move(*rejected));
            return httplib::Server::HandlerResponse::Handled;
        }
        return httplib::Server::HandlerResponse::Unhandled;
    });
    impl.svr.Post(path, [&impl](const httplib::Request& req, httplib::Response& res) {
        try {
            write_response(req, res, impl.endpoint.handle(to_request(req)));
        } catch (const std::exception& e) {
            spdlog::error("HTTP handler failed: {}", e.what());
            res.status = 500;
            res.set_content(R"({"error_code":"INTERNAL_ERROR","message":"internal error"})", "application/json");
        }
    });
    impl.svr.set_error_handler([](const httplib::Request& req, httplib::Response& res) {
        if (!res.body.empty() || res.status < 400) return;
        // Payload too large / bad request raised by the library itself: same JSON shape as ours.
        res.set_content("{\"error_code\":\"HTTP_" + std::to_string(res.status) + "\",\"message\":\"request rejected\"}",
                        "application/json");
        (void)req;
    });
}

McpHttpServer::~McpHttpServer() = default;

bool McpHttpServer::bind(std::string* error) {
    if (impl_->bound) return true;
    int port = config_.port;
    if (port == 0) {
        port = impl_->svr.bind_to_any_port(config_.host);
    } else if (!impl_->svr.bind_to_port(config_.host, port)) {
        port = -1;
    }
    if (port <= 0) {
        if (error) *error = "cannot bind " + config_.host + ":" + std::to_string(config_.port) + " (port in use or address invalid)";
        return false;
    }
    impl_->bound_port = port;
    impl_->bound = true;
    return true;
}

int McpHttpServer::port() const { return impl_->bound_port; }
HttpEndpoint& McpHttpServer::endpoint() { return impl_->endpoint; }
CallExecutor& McpHttpServer::executor() { return impl_->executor; }

int McpHttpServer::run() {
    std::string error;
    if (!bind(&error)) {
        spdlog::error("{}", error);
        return 2;
    }
    std::atomic<bool> listener_done{false};
    std::thread listener([this, &listener_done] {
        impl_->svr.listen_after_bind();
        listener_done = true;
    });
    running_ = true;
    impl_->executor.run();  // main thread: COM lives here
    running_ = false;
    // svr.stop() before listen_after_bind() has started is lost: repeat until the listener returned.
    while (!listener_done) {
        impl_->svr.stop();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    listener.join();
    return 0;
}

std::vector<std::string> McpHttpServer::posture_lines() const {
    std::vector<std::string> lines;
    const bool loopback = is_loopback_bind(config_.host);
    lines.push_back("fairyfly MCP server (HTTP)");
    lines.push_back("  endpoint:       http://" + config_.host + ":" + std::to_string(impl_->bound ? impl_->bound_port : config_.port) +
                    config_.endpoint.path + "   (plain HTTP: put TLS in front, e.g. IIS reverse proxy)");
    lines.push_back(std::string("  mode:           ") + (read_only_.load() ? "read-only guard (write tools hidden and refused)"
                                                                          : "WRITE MODE (state-changing tools enabled)") +
                    (config_.read_only_cap ? " [FAIRYFLY_READ_ONLY cap active]" : ""));
    lines.push_back(std::string("  auth:           ") + (config_.insecure_no_auth ? "NONE (--insecure-no-auth)" : "bearer tokens via authenticator"));
    lines.push_back(std::string("  binding:        ") + config_.host + (loopback ? " (loopback only)" : ""));
    lines.push_back(std::string("  sse:            ") + (config_.endpoint.sse ? "on (Accept: text/event-stream on tools/call)" : "off"));
    std::string hosts = "loopback";
    for (const auto& h : config_.endpoint.allowed_hosts) hosts += ", " + h;
    lines.push_back("  allowed hosts:  " + hosts);
    std::string origins;
    for (const auto& o : config_.endpoint.cors_origins) origins += (origins.empty() ? "" : ", ") + o;
    lines.push_back("  cors origins:   " + (origins.empty() ? std::string("none (no CORS headers; requests with an Origin are rejected)") : origins));
    lines.push_back("  proxy trust:    X-Forwarded-For/Proto are honoured only when the authenticator accepts the proxy secret");
    lines.push_back("  protocol eras:  stateless 2026-07-28 (server/discover, _meta); legacy 2025-11-25, 2025-06-18 (no Mcp-Session-Id)");
    if (!loopback)
        lines.push_back("  WARNING: listening on non-loopback address '" + config_.host +
                        "': the SAP session is reachable from the network; require a TLS reverse proxy, an IP allowlist and tokens");
    if (config_.insecure_no_auth)
        lines.push_back("  WARNING: authentication is disabled; anyone who can reach this port can drive SAP");
    if (!config_.read_only && !config_.read_only_cap)
        lines.push_back("  WARNING: write mode is on: authenticated callers can change SAP data");
    for (const auto& w : impl_->auth->posture_warnings()) lines.push_back("  WARNING: " + w);
    return lines;
}

ServerStatus McpHttpServer::status() const {
    ServerStatus s;
    s.running = running_.load() && !stop_.load();
    s.endpoint = "http://" + config_.host + ":" + std::to_string(impl_->bound ? impl_->bound_port : config_.port) + config_.endpoint.path;
    s.read_only = read_only_.load();
    for (const auto& line : posture_lines())
        if (line.find("WARNING:") != std::string::npos) s.warnings.push_back(line.substr(line.find("WARNING:") + 9));
    s.calls_total = impl_->endpoint.calls_total();
    s.calls_denied = impl_->endpoint.calls_denied();
    return s;
}

void McpHttpServer::set_read_only(bool read_only) {
    if (!read_only && config_.read_only_cap) {
        spdlog::warn("FAIRYFLY_READ_ONLY is set: write mode cannot be enabled");
        return;
    }
    if (read_only_.exchange(read_only) == read_only) return;
    if (!impl_->apply_read_only) return;
    // Applied on the main thread, between calls: subsequent calls use the new mode.
    ExecJob job;
    auto apply = impl_->apply_read_only;
    job.run = [apply, read_only](CallState&) {
        try {
            apply(read_only);
        } catch (const std::exception& e) {
            spdlog::error("switching read-only mode failed: {}", e.what());
        }
        return json();
    };
    impl_->executor.submit(std::move(job));
}

void McpHttpServer::request_stop() {
    stop_ = true;
    impl_->executor.request_stop();
    impl_->svr.stop();
}

void McpHttpServer::request_restart() {
    restart_ = true;
    request_stop();
}

// ---- run_mcp_http ---------------------------------------------------------------------------------

namespace {
std::atomic<McpHttpServer*> g_console_target{nullptr};
#ifdef _WIN32
BOOL WINAPI console_ctrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        if (McpHttpServer* s = g_console_target.load()) s->request_stop();
        return TRUE;
    }
    return FALSE;
}
#endif
} // namespace

int run_mcp_http(HttpRunArgs args, bool* restart_requested) {
    if (restart_requested) *restart_requested = false;
    auto log = args.log_line ? args.log_line : [](const std::string& line) { std::cerr << line << std::endl; };
    const ServeOptions& o = args.options;

    auto provider = std::make_shared<ReloadableProvider>(args.make_provider(args.read_only));

    HttpServerConfig config;
    config.host = o.host.empty() ? std::string("127.0.0.1") : o.host;
    config.port = o.port > 0 ? o.port : 8383;
    config.insecure_no_auth = o.insecure_no_auth;
    config.read_only = args.read_only;
    config.read_only_cap = args.read_only_cap;
    config.call_timeout_ms = args.server_options.call_timeout_ms;
    config.max_queue = args.server_options.max_queue;
    config.endpoint.sse = o.sse;
    config.endpoint.allowed_hosts = o.allowed_hosts;
    config.endpoint.cors_origins = o.cors_origins;
    config.endpoint.server = args.server_options;

    auto authenticator = args.authenticator ? std::move(args.authenticator) : make_http_authenticator(o.insecure_no_auth);

    auto make_provider = args.make_provider;
    auto apply_handler = args.apply_read_only;
    McpHttpServer server(config, *provider, std::move(authenticator), [provider, make_provider, apply_handler](bool ro) {
        if (apply_handler) apply_handler(ro);
        provider->reset(make_provider(ro));
    });

    std::string error;
    if (!server.bind(&error)) {
        Result failure;
        failure.status = Result::Status::Error;
        failure.error["code"] = "BIND_FAILED";
        failure.error["message"] = error;
        log(failure.to_json().dump());
        return 2;
    }
    for (const auto& line : server.posture_lines()) log(line);
    if (args.on_control) args.on_control(server);

#ifdef _WIN32
    g_console_target = &server;
    SetConsoleCtrlHandler(console_ctrl, TRUE);
#endif
    const int code = server.run();
#ifdef _WIN32
    SetConsoleCtrlHandler(console_ctrl, FALSE);
    g_console_target = nullptr;
#endif
    if (restart_requested) *restart_requested = server.restart_requested();
    return code;
}

} // namespace fairyfly::mcp
