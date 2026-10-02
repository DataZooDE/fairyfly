// http.sys (HTTP Server API v2) adapter around the pure HttpEndpoint. Request/response mapping lives in
// http_sys_mapping.cpp; this file owns the kernel objects, the receive workers, body/SSE I/O and the lifecycle.
#include "include/mcp/http_sys_mapping.h"  // winsock2, ws2tcpip, windows, http (in that order)
#include "include/mcp/http_server.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <iostream>
#include <cstdio>
#include <mutex>
#include <thread>

#include <spdlog/spdlog.h>

#include "include/core.h"
#include "include/mcp/authenticators.h"

namespace fairyfly::mcp {

namespace {

using namespace httpsys;

constexpr std::size_t kInitialRequestBuffer = 64 * 1024;
constexpr ULONG kQueueLength = 256;
constexpr std::size_t kBodyReadChunk = 16 * 1024;

// ---- process-wide HttpInitialize reference count -------------------------------------------------------

std::mutex g_init_mutex;
int g_init_count = 0;

ULONG acquire_httpapi() {
    std::lock_guard<std::mutex> lock(g_init_mutex);
    if (g_init_count == 0) {
        const ULONG rc = ::HttpInitialize(HTTPAPI_VERSION_2, HTTP_INITIALIZE_SERVER, nullptr);
        if (rc != NO_ERROR) return rc;
    }
    ++g_init_count;
    return NO_ERROR;
}

void release_httpapi() {
    std::lock_guard<std::mutex> lock(g_init_mutex);
    if (g_init_count > 0 && --g_init_count == 0) ::HttpTerminate(HTTP_INITIALIZE_SERVER, nullptr);
}

bool is_wildcard_host(const std::string& host) {
    return host.empty() || host == "+" || host == "*" || host == "0.0.0.0";
}

std::string win_error(ULONG code) {
    char buf[256] = {};
    const DWORD n = ::FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0, buf, sizeof(buf), nullptr);
    std::string text(buf, n);
    while (!text.empty() && (text.back() == '\r' || text.back() == '\n' || text.back() == ' ' || text.back() == '.')) text.pop_back();
    return text.empty() ? "error " + std::to_string(code) : text + " (" + std::to_string(code) + ")";
}

// ---- sending ---------------------------------------------------------------------------------------------

struct Conn {
    HANDLE queue = nullptr;
    HTTP_REQUEST_ID id = 0;
    HTTP_CONNECTION_ID connection = 0;
};

/// Sends status + headers (+ body unless `more_data`). more_data leaves the response open for entity chunks.
ULONG send_response(const Conn& c, const HttpResponse& r, bool disconnect, bool more_data, bool chunked) {
    const PackedHeaders packed = pack_response_headers(r, chunked);
    HTTP_RESPONSE resp{};
    resp.StatusCode = static_cast<USHORT>(r.status);
    const char* reason = status_text(r.status);
    resp.pReason = reason;
    resp.ReasonLength = static_cast<USHORT>(std::strlen(reason));
    for (const auto& k : packed.known) {
        HTTP_KNOWN_HEADER& h = resp.Headers.KnownHeaders[k.first];
        h.pRawValue = k.second.c_str();
        h.RawValueLength = static_cast<USHORT>(std::min<std::size_t>(k.second.size(), 0xFFFF));
    }
    std::vector<HTTP_UNKNOWN_HEADER> unknown;
    unknown.reserve(packed.unknown.size());
    for (const auto& u : packed.unknown) {
        HTTP_UNKNOWN_HEADER h{};
        h.pName = u.first.c_str();
        h.NameLength = static_cast<USHORT>(std::min<std::size_t>(u.first.size(), 0xFFFF));
        h.pRawValue = u.second.c_str();
        h.RawValueLength = static_cast<USHORT>(std::min<std::size_t>(u.second.size(), 0xFFFF));
        unknown.push_back(h);
    }
    if (!unknown.empty()) {
        resp.Headers.pUnknownHeaders = unknown.data();
        resp.Headers.UnknownHeaderCount = static_cast<USHORT>(unknown.size());
    }
    HTTP_DATA_CHUNK chunk{};
    if (!more_data && !r.body.empty()) {
        chunk.DataChunkType = HttpDataChunkFromMemory;
        chunk.FromMemory.pBuffer = const_cast<char*>(r.body.data());
        chunk.FromMemory.BufferLength = static_cast<ULONG>(r.body.size());
        resp.EntityChunkCount = 1;
        resp.pEntityChunks = &chunk;
    }
    ULONG flags = 0;
    if (disconnect) flags |= HTTP_SEND_RESPONSE_FLAG_DISCONNECT;
    if (more_data) flags |= HTTP_SEND_RESPONSE_FLAG_MORE_DATA;
    ULONG sent = 0;
    return ::HttpSendHttpResponse(c.queue, c.id, flags, &resp, nullptr, &sent, nullptr, 0, nullptr, nullptr);
}

HttpResponse json_error(int status, const char* code, const char* message) {
    HttpResponse r;
    r.status = status;
    r.body = std::string("{\"error_code\":\"") + code + "\",\"message\":\"" + message + "\"}";
    r.set_header("Content-Type", "application/json");
    r.set_header("Cache-Control", "no-store");
    return r;
}

/// SseSink over http.sys entity-body sends. connected() uses one overlapped HttpWaitForDisconnectEx.
class HttpSysSink : public SseSink {
public:
    /// `framed`: HTTP/1.1 response with "Transfer-Encoding: chunked": http.sys does NOT frame chunks itself when the
    /// application sets that header, so every write is wrapped as "<hex size>CRLF<data>CRLF" here.
    HttpSysSink(const Conn& c, bool framed) : conn_(c), framed_(framed) {}
    ~HttpSysSink() override { stop_wait(); }

    bool write(std::string_view frame) override {
        if (failed_) return false;
        if (frame.empty()) return true;  // a zero-length chunk would end the message
        std::string wrapped;
        if (framed_) {
            char size[24];
            std::snprintf(size, sizeof(size), "%zx\r\n", frame.size());
            wrapped.reserve(frame.size() + 32);
            wrapped += size;
            wrapped.append(frame.data(), frame.size());
            wrapped += "\r\n";
            frame = wrapped;
        }
        HTTP_DATA_CHUNK chunk{};
        chunk.DataChunkType = HttpDataChunkFromMemory;
        chunk.FromMemory.pBuffer = const_cast<char*>(frame.data());
        chunk.FromMemory.BufferLength = static_cast<ULONG>(frame.size());
        ULONG sent = 0;
        // MORE_DATA without BUFFER_DATA: http.sys puts the chunk on the wire before returning (immediate flush).
        const ULONG rc = ::HttpSendResponseEntityBody(conn_.queue, conn_.id, HTTP_SEND_RESPONSE_FLAG_MORE_DATA, 1, &chunk,
                                                      &sent, nullptr, 0, nullptr, nullptr);
        if (rc != NO_ERROR) {
            failed_ = true;
            return false;
        }
        return true;
    }

    bool connected() const override {
        if (failed_ || gone_) return false;
        if (state_ == State::NotStarted) start_wait();
        if (state_ == State::Pending && ::WaitForSingleObject(wait_->event, 0) == WAIT_OBJECT_0) {
            gone_ = true;
            return false;
        }
        return true;  // also when the wait could not be started: a failing keep-alive write ends the stream
    }

    bool failed() const { return failed_; }
    bool framed() const { return framed_; }
    bool gone() const { return gone_; }

    /// Cancel the pending disconnect wait and wait for its completion before the OVERLAPPED is freed.
    void stop_wait() {
        if (state_ != State::Pending || !wait_) return;
        if (::WaitForSingleObject(wait_->event, 0) != WAIT_OBJECT_0) {
            ::CancelIoEx(conn_.queue, &wait_->ov);
            if (::WaitForSingleObject(wait_->event, 5000) != WAIT_OBJECT_0) {
                (void)wait_.release();  // never free an OVERLAPPED the kernel may still write to
                state_ = State::Done;
                return;
            }
        }
        state_ = State::Done;
    }

private:
    enum class State { NotStarted, Pending, Done };
    struct WaitState {
        OVERLAPPED ov{};
        HANDLE event = nullptr;
        ~WaitState() {
            if (event) ::CloseHandle(event);
        }
    };

    void start_wait() const {
        wait_ = std::make_unique<WaitState>();
        wait_->event = ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
        if (!wait_->event) {
            state_ = State::Done;
            return;
        }
        wait_->ov.hEvent = wait_->event;
        const ULONG rc = ::HttpWaitForDisconnectEx(conn_.queue, conn_.connection, 0, &wait_->ov);
        if (rc == ERROR_IO_PENDING) {
            state_ = State::Pending;
        } else if (rc == NO_ERROR) {
            gone_ = true;  // completed synchronously: the connection is already gone
            state_ = State::Done;
        } else {
            state_ = State::Done;  // cannot observe disconnects; write failures still end the stream
        }
    }

    Conn conn_;
    bool framed_ = false;
    mutable State state_ = State::NotStarted;
    mutable std::unique_ptr<WaitState> wait_;
    mutable bool gone_ = false;
    bool failed_ = false;
};

enum class BodyResult { Ok, TooLarge, Error, Slow, Busy };

// A caller with a valid token must not be able to pin every receive worker with a drip-fed upload: the kernel
// EntityBody timer restarts whenever bytes arrive, so we bound the TOTAL time of one body and the number of
// workers that may sit in body reads at the same time (the rest of the pool stays free for new requests).
constexpr std::chrono::seconds kBodyTotalDeadline{20};
constexpr int kMaxConcurrentBodyReaders = 8;
std::atomic<int> g_body_readers{0};

struct BodyReaderSlot {
    bool acquired;
    BodyReaderSlot() : acquired(g_body_readers.fetch_add(1) < kMaxConcurrentBodyReaders) {
        if (!acquired) g_body_readers.fetch_sub(1);
    }
    ~BodyReaderSlot() { if (acquired) g_body_readers.fetch_sub(1); }
    BodyReaderSlot(const BodyReaderSlot&) = delete;
    BodyReaderSlot& operator=(const BodyReaderSlot&) = delete;
};

BodyResult read_body(const Conn& c, std::size_t cap, std::string& out) {
    BodyReaderSlot slot;
    if (!slot.acquired) return BodyResult::Busy;
    const auto deadline = std::chrono::steady_clock::now() + kBodyTotalDeadline;
    std::vector<char> buf(kBodyReadChunk);
    while (true) {
        ULONG got = 0;
        const ULONG rc = ::HttpReceiveRequestEntityBody(c.queue, c.id, 0, buf.data(), static_cast<ULONG>(buf.size()), &got, nullptr);
        if (rc == NO_ERROR || rc == ERROR_HANDLE_EOF) {
            if (got > 0) {
                if (out.size() + got > cap) return BodyResult::TooLarge;
                out.append(buf.data(), got);
            }
            if (rc == ERROR_HANDLE_EOF) return BodyResult::Ok;
            if (std::chrono::steady_clock::now() > deadline) return BodyResult::Slow;
            continue;
        }
        return BodyResult::Error;
    }
}

} // namespace

// ---- public helpers ------------------------------------------------------------------------------------

std::string http_endpoint_url(bool tls, const std::string& host, const std::vector<std::string>& allowed_hosts, int port,
                              const std::string& path) {
    std::string shown = host.empty() ? std::string("127.0.0.1") : host;
    if (tls) {
        if (is_wildcard_host(shown)) shown = allowed_hosts.empty() ? std::string("+") : allowed_hosts.front();
    } else if (shown == "localhost") {
        shown = "127.0.0.1";
    } else if (is_wildcard_host(shown)) {
        shown = "+";
    }
    if (shown.find(':') != std::string::npos && shown.front() != '[') shown = "[" + shown + "]";
    return std::string(tls ? "https://" : "http://") + shown + ":" + std::to_string(port) + path;
}

// ---- Impl --------------------------------------------------------------------------------------------------

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

    bool bound = false;
    int bound_port = 0;
    std::string prefix;
    std::string bind_reason;
    bool http_initialized = false;
    HTTP_SERVER_SESSION_ID session = 0;
    HTTP_URL_GROUP_ID group = 0;
    bool url_added = false;
    std::mutex queue_mutex;  ///< guards `queue` against request_stop() racing bind()/close
    HANDLE queue = nullptr;
    std::atomic<bool> stopping{false};

    void shutdown_queue() {
        std::lock_guard<std::mutex> lock(queue_mutex);
        if (queue) ::HttpShutdownRequestQueue(queue);
    }

    /// Teardown order: URLs, URL group, server session, request queue, HttpTerminate.
    void close_all() {
        if (group && url_added) ::HttpRemoveUrlFromUrlGroup(group, nullptr, HTTP_URL_FLAG_REMOVE_ALL);
        url_added = false;
        if (group) ::HttpCloseUrlGroup(group);
        group = 0;
        if (session) ::HttpCloseServerSession(session);
        session = 0;
        {
            std::lock_guard<std::mutex> lock(queue_mutex);
            if (queue) ::HttpCloseRequestQueue(queue);
            queue = nullptr;
        }
        if (http_initialized) release_httpapi();
        http_initialized = false;
        bound = false;
    }

    void handle_request(const HTTP_REQUEST& req);
    void worker_loop();
};

void McpHttpServer::Impl::handle_request(const HTTP_REQUEST& req) {
    Conn conn;
    conn.queue = queue;
    conn.id = req.RequestId;
    conn.connection = req.ConnectionId;
    bool responded = false;
    try {
        HttpRequest request = map_request(req);
        const bool body_pending = has_entity_body(req);
        const std::size_t cap = endpoint.max_body_bytes();

        // Header-only decision first: nothing of a rejected request's body is ever read or drained.
        const ContentLength length = parse_content_length(request.header("Content-Length"));
        if (length.present && !length.valid) {
            responded = true;
            send_response(conn, json_error(400, "BAD_REQUEST", "invalid Content-Length"), true, false, false);
            return;
        }
        HttpEndpoint::PreAuth pre = endpoint.preauthenticate(request);
        if (pre.rejection) {
            responded = true;
            send_response(conn, *pre.rejection, body_pending, false, false);
            return;
        }
        if (body_pending) {
            std::string body;
            const BodyResult result = read_body(conn, cap, body);
            if (result != BodyResult::Ok) {
                responded = true;
                if (result == BodyResult::TooLarge)
                    send_response(conn, json_error(413, "PAYLOAD_TOO_LARGE", "request body too large"), true, false, false);
                else if (result == BodyResult::Slow)
                    send_response(conn, json_error(408, "REQUEST_TIMEOUT", "request body took too long"), true, false, false);
                else if (result == BodyResult::Busy)
                    send_response(conn, json_error(503, "BUSY", "too many uploads in progress, retry shortly"), true, false, false);
                else
                    send_response(conn, json_error(400, "BAD_REQUEST", "request body could not be read"), true, false, false);
                return;
            }
            request.body = std::move(body);
        }
        HttpResponse response = endpoint.handle_authenticated(request, pre.principal);
        responded = true;
        if (!response.stream) {
            send_response(conn, response, false, false, false);
            return;
        }
        // ---- SSE ----
        const bool chunked = is_http11(req);  // HTTP/1.1 only (h2 frames DATA itself); we frame the chunks
        if (send_response(conn, response, false, true, chunked) != NO_ERROR) return;
        HttpSysSink sink(conn, chunked);
        try {
            response.stream(sink);
        } catch (const std::exception& e) {
            spdlog::error("SSE stream failed: {}", e.what());
        }
        sink.stop_wait();
        ULONG sent = 0;
        const bool abort = sink.failed() || sink.gone();
        if (abort || !chunked) {
            // HTTP/1.0 without framing ends by closing; an aborted stream must not look complete either.
            ::HttpSendResponseEntityBody(conn.queue, conn.id, HTTP_SEND_RESPONSE_FLAG_DISCONNECT, 0, nullptr, &sent, nullptr, 0,
                                         nullptr, nullptr);
        } else {
            static const char kLastChunk[] = "0\r\n\r\n";
            HTTP_DATA_CHUNK last{};
            last.DataChunkType = HttpDataChunkFromMemory;
            last.FromMemory.pBuffer = const_cast<char*>(kLastChunk);
            last.FromMemory.BufferLength = sizeof(kLastChunk) - 1;
            ::HttpSendResponseEntityBody(conn.queue, conn.id, 0, 1, &last, &sent, nullptr, 0, nullptr, nullptr);
        }
    } catch (const std::exception& e) {
        spdlog::error("HTTP handler failed: {}", e.what());
        if (!responded) send_response(conn, json_error(500, "INTERNAL_ERROR", "internal error"), true, false, false);
    }
}

void McpHttpServer::Impl::worker_loop() {
    std::vector<std::uint64_t> buffer(kInitialRequestBuffer / sizeof(std::uint64_t));
    HTTP_REQUEST_ID request_id = 0;
    while (!stopping.load()) {
        ULONG bytes = 0;
        auto* req = reinterpret_cast<PHTTP_REQUEST>(buffer.data());
        const ULONG rc = ::HttpReceiveHttpRequest(queue, request_id, 0, req, static_cast<ULONG>(buffer.size() * sizeof(std::uint64_t)),
                                                  &bytes, nullptr);
        if (rc == NO_ERROR) {
            request_id = 0;
            handle_request(*req);
        } else if (rc == ERROR_MORE_DATA) {
            request_id = req->RequestId;  // same request, bigger buffer
            buffer.resize((bytes + sizeof(std::uint64_t) - 1) / sizeof(std::uint64_t));
        } else if (rc == ERROR_CONNECTION_INVALID && request_id != 0) {
            request_id = 0;  // the client vanished while the buffer was regrown
        } else if (rc == ERROR_OPERATION_ABORTED || rc == ERROR_INVALID_HANDLE || rc == ERROR_HANDLE_EOF) {
            break;  // HttpShutdownRequestQueue / queue closed
        } else {
            spdlog::warn("HttpReceiveHttpRequest failed: {}", win_error(rc));
            request_id = 0;
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }
}

// ---- McpHttpServer -------------------------------------------------------------------------------------------

McpHttpServer::McpHttpServer(HttpServerConfig config, ToolProvider& provider,
                             std::unique_ptr<IAuthenticator> authenticator,
                             std::function<void(bool)> apply_read_only)
    : impl_(std::make_unique<Impl>(config, provider, std::move(authenticator), std::move(apply_read_only))),
      config_(std::move(config)), read_only_(config_.read_only) {}

McpHttpServer::~McpHttpServer() {
    impl_->close_all();
}

const std::string& McpHttpServer::bind_error_reason() const { return impl_->bind_reason; }
const std::string& McpHttpServer::prefix() const { return impl_->prefix; }

bool McpHttpServer::bind(std::string* error) {
    Impl& s = *impl_;
    if (s.bound) return true;
    const auto fail = [&](const std::string& reason, const std::string& message) {
        s.bind_reason = reason;
        if (error) *error = message;
        s.close_all();
        return false;
    };

    if (config_.port == 0) return fail("invalid_prefix", "http.sys has no ephemeral ports: configure a fixed port");
    if (!config_.tls && !is_loopback_host(config_.host) && !config_.insecure_http)
        return fail("insecure_bind", "plain HTTP on non-loopback host '" + config_.host +
                                         "' is refused: use TLS (fairyfly mcp setup) or pass --insecure-http");
    std::string prefix_error;
    const std::string prefix =
        make_prefix(config_.tls ? "https" : "http", config_.host, config_.port, config_.endpoint.path, &prefix_error);
    if (prefix.empty()) return fail("invalid_prefix", "invalid http.sys prefix: " + prefix_error);

    ULONG rc = acquire_httpapi();
    if (rc != NO_ERROR) return fail("http_sys_error", "HttpInitialize failed: " + win_error(rc));
    s.http_initialized = true;

    rc = ::HttpCreateServerSession(HTTPAPI_VERSION_2, &s.session, 0);
    if (rc != NO_ERROR) return fail("http_sys_error", "HttpCreateServerSession failed: " + win_error(rc));
    rc = ::HttpCreateUrlGroup(s.session, &s.group, 0);
    if (rc != NO_ERROR) return fail("http_sys_error", "HttpCreateUrlGroup failed: " + win_error(rc));
    {
        std::lock_guard<std::mutex> lock(s.queue_mutex);
        rc = ::HttpCreateRequestQueue(HTTPAPI_VERSION_2, nullptr, nullptr, 0, &s.queue);
    }
    if (rc != NO_ERROR) return fail("http_sys_error", "HttpCreateRequestQueue failed: " + win_error(rc));

    HTTP_BINDING_INFO binding{};
    binding.Flags.Present = 1;
    binding.RequestQueueHandle = s.queue;
    rc = ::HttpSetUrlGroupProperty(s.group, HttpServerBindingProperty, &binding, sizeof(binding));
    if (rc != NO_ERROR) return fail("http_sys_error", "binding the URL group to the request queue failed: " + win_error(rc));

    // Kernel timeouts replace application-level drain logic (slow-loris defence).
    HTTP_TIMEOUT_LIMIT_INFO timeouts{};
    timeouts.Flags.Present = 1;
    timeouts.HeaderWait = 10;
    timeouts.EntityBody = 15;
    timeouts.DrainEntityBody = 5;
    timeouts.RequestQueue = 30;
    timeouts.IdleConnection = 120;
    timeouts.MinSendRate = 150;
    rc = ::HttpSetUrlGroupProperty(s.group, HttpServerTimeoutsProperty, &timeouts, sizeof(timeouts));
    if (rc != NO_ERROR) return fail("http_sys_error", "setting http.sys timeouts failed: " + win_error(rc));

    // Also on the server session. Measured (issue 'half-header'): neither call changes the lifetime of a socket that
    // has sent only part of a header (or nothing): until a request is routed to the URL group, http.sys uses the
    // machine-wide timers (netsh http show timeout, default 120 s). EntityBody/DrainEntityBody/MinSendRate do apply.
    // Not fatal when refused.
    rc = ::HttpSetServerSessionProperty(s.session, HttpServerTimeoutsProperty, &timeouts, sizeof(timeouts));
    if (rc != NO_ERROR) spdlog::warn("HttpSetServerSessionProperty(timeouts) failed: {}", win_error(rc));

    ULONG queue_length = kQueueLength;
    rc = ::HttpSetRequestQueueProperty(s.queue, HttpServerQueueLengthProperty, &queue_length, sizeof(queue_length), 0, nullptr);
    if (rc != NO_ERROR) spdlog::warn("HttpSetRequestQueueProperty(queue length) failed: {}", win_error(rc));
    HTTP_503_RESPONSE_VERBOSITY verbosity = Http503ResponseVerbosityLimited;
    rc = ::HttpSetRequestQueueProperty(s.queue, HttpServer503VerbosityProperty, &verbosity, sizeof(verbosity), 0, nullptr);
    if (rc != NO_ERROR) spdlog::warn("HttpSetRequestQueueProperty(503 verbosity) failed: {}", win_error(rc));

    const std::wstring wide_prefix = utf8_to_wide(prefix);
    rc = ::HttpAddUrlToUrlGroup(s.group, wide_prefix.c_str(), 0, 0);
    if (rc == ERROR_ACCESS_DENIED)
        return fail("no_url_reservation", "no URL reservation for " + prefix + " (access denied): run 'fairyfly mcp setup' "
                                              "(or 'fairyfly mcp setup --no-tls' for plain-HTTP development)");
    if (rc == ERROR_ALREADY_EXISTS || rc == ERROR_SHARING_VIOLATION)
        return fail("prefix_registered", prefix + " is already registered by another process: another fairyfly instance "
                                                   "or the tray app is running");
    if (rc == ERROR_INVALID_PARAMETER)
        return fail("invalid_prefix", "http.sys rejected the prefix " + prefix);
    if (rc != NO_ERROR) return fail("http_sys_error", "HttpAddUrlToUrlGroup(" + prefix + ") failed: " + win_error(rc));
    s.url_added = true;

    s.prefix = prefix;
    s.bound_port = config_.port;
    s.bound = true;
    s.bind_reason.clear();
    return true;
}

int McpHttpServer::port() const { return impl_->bound_port; }
HttpEndpoint& McpHttpServer::endpoint() { return impl_->endpoint; }
CallExecutor& McpHttpServer::executor() { return impl_->executor; }

int McpHttpServer::run() {
    if (stop_.load()) return 0;  // stop requested before run() (tray: stop-before-run)
    std::string error;
    if (!bind(&error)) {
        spdlog::error("{}", error);
        return 2;
    }
    Impl& s = *impl_;
    const int workers = std::max(2, config_.worker_threads);
    std::vector<std::thread> pool;
    pool.reserve(static_cast<std::size_t>(workers));
    for (int i = 0; i < workers; ++i) pool.emplace_back([&s] { s.worker_loop(); });
    running_ = true;
    if (!stop_.load()) s.executor.run();  // calling thread: COM lives here
    running_ = false;
    s.stopping = true;
    s.shutdown_queue();  // pending receives return ERROR_OPERATION_ABORTED
    for (auto& t : pool) t.join();
    return 0;
}

std::vector<std::string> McpHttpServer::posture_lines() const {
    std::vector<std::string> lines;
    const bool loopback = is_loopback_host(config_.host);
    const int shown_port = impl_->bound ? impl_->bound_port : config_.port;
    lines.push_back("fairyfly MCP server (HTTP)");
    lines.push_back("  endpoint:       " + http_endpoint_url(config_.tls, config_.host, config_.endpoint.allowed_hosts, shown_port,
                                                              config_.endpoint.path));
    lines.push_back(std::string("  tls:            ") + (config_.tls ? "http.sys (certificate bound by 'fairyfly mcp setup')"
                                                                     : "plain HTTP (loopback only)"));
    std::string allow;
    for (const auto& a : config_.allow_ip) allow += (allow.empty() ? "" : ", ") + a;
    lines.push_back("  client ip allow-list: " + (allow.empty() ? std::string("any (loopback always allowed)") : allow));
    lines.push_back(std::string("  mode:           ") + (read_only_.load() ? "read-only guard (write tools hidden and refused)"
                                                                          : "WRITE MODE (state-changing tools enabled)") +
                    (config_.read_only_cap ? " [FAIRYFLY_READ_ONLY cap active]" : ""));
    lines.push_back(std::string("  auth:           ") + (config_.insecure_no_auth ? "NONE (--insecure-no-auth)" : "bearer tokens via authenticator"));
    lines.push_back(std::string("  sse:            ") + (config_.endpoint.sse ? "on (tools/call: only when the client prefers text/event-stream or sends a progressToken)" : "off"));
    std::string hosts = "loopback";
    for (const auto& h : config_.endpoint.allowed_hosts) hosts += ", " + h;
    lines.push_back("  allowed hosts:  " + hosts);
    std::string origins;
    for (const auto& o : config_.endpoint.cors_origins) origins += (origins.empty() ? "" : ", ") + o;
    lines.push_back("  cors origins:   " + (origins.empty() ? std::string("none (no CORS headers; requests with an Origin are rejected)") : origins));
    lines.push_back("  protocol eras:  stateless 2026-07-28 (server/discover, _meta); legacy 2025-11-25, 2025-06-18 (no Mcp-Session-Id)");
    if (!loopback)
        lines.push_back("  WARNING: listening on non-loopback address '" + config_.host +
                        "': reachable from the network: " + (config_.tls ? "TLS on, " : "plain HTTP, ") + "tokens required" +
                        (config_.allow_ip.empty() ? ", consider --allow-ip" : ""));
    const std::vector<std::string> auth_warnings = impl_->auth->posture_warnings();
    // The insecure-auth warning comes from the server and from AllowAllAuthenticator: print the topic once.
    const auto mentions_disabled_auth = [](const std::string& w) {
        std::string lower = w;
        std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return lower.find("authentication disabled") != std::string::npos || lower.find("authentication is disabled") != std::string::npos ||
               lower.find("--insecure-no-auth") != std::string::npos;
    };
    const bool auth_reports_disabled = std::any_of(auth_warnings.begin(), auth_warnings.end(), mentions_disabled_auth);
    if (config_.insecure_no_auth && !auth_reports_disabled)
        lines.push_back("  WARNING: authentication is disabled; anyone who can reach this port can drive SAP");
    if (!config_.read_only && !config_.read_only_cap)
        lines.push_back("  WARNING: write mode is on: authenticated callers can change SAP data");
    for (const auto& w : auth_warnings) {
        const std::string line = "  WARNING: " + w;
        if (std::find(lines.begin(), lines.end(), line) == lines.end()) lines.push_back(line);
    }
    return lines;
}

ServerStatus McpHttpServer::status() const {
    ServerStatus s;
    s.running = running_.load() && !stop_.load();
    s.endpoint = http_endpoint_url(config_.tls, config_.host, config_.endpoint.allowed_hosts,
                                   impl_->bound ? impl_->bound_port : config_.port, config_.endpoint.path);
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
    impl_->stopping = true;
    impl_->shutdown_queue();
}

void McpHttpServer::request_restart() {
    restart_ = true;
    request_stop();
}

// ---- run_mcp_http ---------------------------------------------------------------------------------

namespace {
std::atomic<McpHttpServer*> g_console_target{nullptr};
BOOL WINAPI console_ctrl(DWORD type) {
    if (type == CTRL_C_EVENT || type == CTRL_BREAK_EVENT || type == CTRL_CLOSE_EVENT) {
        if (McpHttpServer* s = g_console_target.load()) s->request_stop();
        return TRUE;
    }
    return FALSE;
}
} // namespace

int run_mcp_http(HttpRunArgs args, bool* restart_requested) {
    if (restart_requested) *restart_requested = false;
    auto log = args.log_line ? args.log_line : [](const std::string& line) { std::cerr << line << std::endl; };
    const ServeOptions& o = args.options;

    auto provider = std::make_shared<ReloadableProvider>(args.make_provider(args.read_only));

    HttpServerConfig config;
    config.host = o.host.empty() ? std::string("127.0.0.1") : o.host;
    config.port = o.port > 0 ? o.port : (o.tls ? 8443 : 8383);
    config.tls = o.tls;
    config.allow_ip = o.allow_ip;
    config.insecure_http = o.insecure_http;
    config.insecure_no_auth = o.insecure_no_auth;
    config.read_only = args.read_only;
    config.read_only_cap = args.read_only_cap;
    config.call_timeout_ms = args.server_options.call_timeout_ms;
    config.max_queue = args.server_options.max_queue;
    config.endpoint.sse = o.sse;
    config.endpoint.allowed_hosts = o.allowed_hosts;
    config.endpoint.cors_origins = o.cors_origins;
    config.endpoint.allow_ip = o.allow_ip;
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
        failure.error["code"] = server.bind_error_reason() == "insecure_bind" ? "INSECURE_BIND" : "BIND_FAILED";
        failure.error["reason"] = server.bind_error_reason();
        failure.error["message"] = error;
        log(failure.to_json().dump());
        return 2;
    }
    for (const auto& line : server.posture_lines()) log(line);
    if (args.on_control) args.on_control(server);

    g_console_target = &server;
    SetConsoleCtrlHandler(console_ctrl, TRUE);
    const int code = server.run();
    SetConsoleCtrlHandler(console_ctrl, FALSE);
    g_console_target = nullptr;
    if (restart_requested) *restart_requested = server.restart_requested();
    return code;
}

} // namespace fairyfly::mcp
