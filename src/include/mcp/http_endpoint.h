#pragma once
// Transport-neutral MCP-over-HTTP endpoint. HttpEndpoint::handle() maps one HttpRequest to one
// HttpResponse with no sockets involved (unit-testable); http_server.cpp wraps it in cpp-httplib.
// Threading: handle() may run on any worker thread; tool work is enqueued on the CallExecutor and
// executed on the main (COM) thread, the worker only waits.

#include <atomic>
#include <cstddef>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/mcp/call_executor.h"
#include "include/mcp/principal.h"
#include "include/mcp/types.h"

namespace fairyfly::mcp {

struct CaseInsensitiveLess {
    bool operator()(const std::string& a, const std::string& b) const;
};
using HeaderMap = std::map<std::string, std::string, CaseInsensitiveLess>;

struct HttpRequest {
    std::string method;     ///< "POST", "GET", ...
    std::string path;       ///< without query string
    HeaderMap headers;
    std::string body;
    std::string peer_addr;  ///< socket peer ("127.0.0.1")

    std::string header(const std::string& name) const {
        auto it = headers.find(name);
        return it == headers.end() ? std::string() : it->second;
    }
};

/// Where an SSE stream writes. write() returns false when the client is gone.
class SseSink {
public:
    virtual ~SseSink() = default;
    virtual bool write(std::string_view frame) = 0;
    virtual bool connected() const { return true; }
};

struct HttpResponse {
    int status = 200;
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    /// When set the response is a server-sent-event stream: the adapter sends status + headers and
    /// then calls this once on the worker thread; it returns when the stream is complete.
    std::function<void(SseSink&)> stream;

    void set_header(std::string name, std::string value);
    std::string header(const std::string& name) const;
};

/// SSE frame formatting (pure). `data` may contain newlines (one "data:" line each).
std::string format_sse_event(std::string_view event, std::string_view data, std::string_view id = {});
std::string format_sse_comment(std::string_view text);

struct HttpEndpointOptions {
    std::string path = "/mcp";
    std::size_t max_body_bytes = 1024 * 1024;
    std::vector<std::string> allowed_hosts;  ///< accepted Host names besides loopback
    std::vector<std::string> cors_origins;   ///< accepted Origin values; empty = reject any Origin
    bool sse = true;
    int keepalive_ms = 15000;                ///< ": keep-alive" comment interval on SSE streams
    int poll_ms = 100;                       ///< SSE / wait poll slice
    ServerOptions server;                    ///< name/version/instructions
};

class HttpEndpoint {
public:
    /// `authenticator` is not owned and must outlive the endpoint (null = deny everything).
    HttpEndpoint(HttpEndpointOptions options, CallExecutor& executor, ToolProvider& provider,
                 IAuthenticator* authenticator);

    /// Cheap header-only checks (path, method, Host, Origin, Content-Type, Content-Length). Returns a
    /// response when the request must be rejected. Used by the adapter's pre-routing handler and
    /// again by handle().
    std::optional<HttpResponse> precheck(const HttpRequest& request) const;

    HttpResponse handle(const HttpRequest& request);

    long long calls_total() const { return calls_total_.load(); }
    long long calls_denied() const { return calls_denied_.load(); }

    /// Protocol versions served over HTTP, newest first.
    static const std::vector<std::string>& supported_versions();

private:
    HttpResponse dispatch(const HttpRequest& request, const Principal& principal);
    HttpResponse finish(const HttpRequest& request, HttpResponse response) const;
    bool host_allowed(const std::string& host_header) const;
    bool origin_allowed(const std::string& origin) const;

    HttpEndpointOptions options_;
    CallExecutor& executor_;
    ToolProvider& provider_;
    IAuthenticator* authenticator_;
    std::atomic<long long> calls_total_{0};
    mutable std::atomic<long long> calls_denied_{0};
};

} // namespace fairyfly::mcp
