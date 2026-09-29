#include "include/mcp/http_endpoint.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>

#include <spdlog/spdlog.h>

#include "include/mcp/authenticators.h"
#include "include/mcp/json_rpc.h"
#include "include/mcp/protocol_session.h"

namespace fairyfly::mcp {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

std::string trim(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string dump(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

HttpResponse json_response(int status, const json& message) {
    HttpResponse r;
    r.status = status;
    r.body = dump(message);
    r.set_header("Content-Type", "application/json");
    return r;
}

/// Non-RPC failure: {"error_code","message"} (auth, routing, limits).
HttpResponse plain_error(int status, const std::string& code, const std::string& message) {
    return json_response(status, json{{"error_code", code}, {"message", message}});
}

HttpResponse empty_response(int status) {
    HttpResponse r;
    r.status = status;
    return r;
}

/// Host name part of a Host header value, lowercased ("127.0.0.1:8383" -> "127.0.0.1", "[::1]:1" -> "[::1]").
std::string host_name(const std::string& header) {
    std::string h = lower(trim(header));
    if (h.empty()) return h;
    if (h.front() == '[') {
        const auto close = h.find(']');
        return close == std::string::npos ? h : h.substr(0, close + 1);
    }
    const auto first = h.find(':');
    if (first != std::string::npos && first == h.rfind(':')) return h.substr(0, first);
    return h;
}

bool is_loopback_host(const std::string& name) {
    return name == "127.0.0.1" || name == "localhost" || name == "[::1]" || name == "::1";
}

std::string strip_slash(std::string s) {
    while (!s.empty() && s.back() == '/') s.pop_back();
    return s;
}

/// params._meta[key], also accepting the "io.modelcontextprotocol/" namespaced spelling.
json meta_value(const json& params, const char* key) {
    if (!params.is_object() || !params.contains("_meta") || !params["_meta"].is_object()) return json();
    const json& meta = params["_meta"];
    if (meta.contains(key)) return meta[key];
    const std::string prefixed = std::string("io.modelcontextprotocol/") + key;
    if (meta.contains(prefixed)) return meta[prefixed];
    return json();
}

bool is_stateless_version(const std::string& v) { return v == kStatelessVersion; }

json supported_json() {
    json a = json::array();
    for (const auto& v : HttpEndpoint::supported_versions()) a.push_back(v);
    return a;
}

json http_capabilities() { return json{{"tools", json{{"listChanged", false}}}}; }

json decorate_stateless(json message, const std::string& method) {
    if (message.is_object() && message.contains("result") && message["result"].is_object()) {
        message["result"]["resultType"] = "complete";
        if (method == "tools/list") {
            message["result"]["ttlMs"] = 30000;
            message["result"]["cacheScope"] = "private";
        }
    }
    return message;
}

/// Rendezvous between the main thread (executor job) and the HTTP worker.
struct Waiter {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::string> frames;  ///< SSE frames produced before the final message (progress)
    json final_message;
    bool done = false;

    void push_frame(std::string frame) {
        {
            std::lock_guard<std::mutex> lock(m);
            frames.push_back(std::move(frame));
        }
        cv.notify_all();
    }
    void finish(const json& message) {
        {
            std::lock_guard<std::mutex> lock(m);
            final_message = message;
            done = true;
        }
        cv.notify_all();
    }
};

std::string progress_frame(const json& token, double progress, std::optional<double> total, const std::string& message) {
    json params{{"progressToken", token}, {"progress", progress}};
    if (total) params["total"] = *total;
    if (!message.empty()) params["message"] = message;
    return format_sse_event("message", dump(json{{"jsonrpc", "2.0"}, {"method", "notifications/progress"}, {"params", params}}));
}

} // namespace

// ---- small public helpers ---------------------------------------------------------------------

bool CaseInsensitiveLess::operator()(const std::string& a, const std::string& b) const {
    return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](unsigned char x, unsigned char y) {
        return std::tolower(x) < std::tolower(y);
    });
}

void HttpResponse::set_header(std::string name, std::string value) {
    for (auto& h : headers)
        if (lower(h.first) == lower(name)) {
            h.second = std::move(value);
            return;
        }
    headers.emplace_back(std::move(name), std::move(value));
}

std::string HttpResponse::header(const std::string& name) const {
    for (const auto& h : headers)
        if (lower(h.first) == lower(name)) return h.second;
    return {};
}

std::string format_sse_event(std::string_view event, std::string_view data, std::string_view id) {
    std::string out;
    if (!id.empty()) out += "id: " + std::string(id) + "\n";
    if (!event.empty()) out += "event: " + std::string(event) + "\n";
    size_t start = 0;
    while (true) {
        const size_t nl = data.find('\n', start);
        std::string_view line = data.substr(start, nl == std::string_view::npos ? std::string_view::npos : nl - start);
        if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
        out += "data: " + std::string(line) + "\n";
        if (nl == std::string_view::npos) break;
        start = nl + 1;
    }
    out += "\n";
    return out;
}

std::string format_sse_comment(std::string_view text) { return ": " + std::string(text) + "\n\n"; }

const std::vector<std::string>& HttpEndpoint::supported_versions() {
    static const std::vector<std::string> versions = {kStatelessVersion, "2025-11-25", "2025-06-18"};
    return versions;
}

// ---- endpoint ---------------------------------------------------------------------------------

HttpEndpoint::HttpEndpoint(HttpEndpointOptions options, CallExecutor& executor, ToolProvider& provider,
                           IAuthenticator* authenticator)
    : options_(std::move(options)), executor_(executor), provider_(provider), authenticator_(authenticator) {
    for (auto& h : options_.allowed_hosts) h = lower(trim(h));
    for (auto& o : options_.cors_origins) o = lower(strip_slash(trim(o)));
}

bool HttpEndpoint::host_allowed(const std::string& host_header) const {
    const std::string name = host_name(host_header);
    if (name.empty()) return false;
    if (is_loopback_host(name)) return true;
    const std::string raw = lower(trim(host_header));
    for (const auto& allowed : options_.allowed_hosts)
        if (allowed == name || allowed == raw) return true;
    return false;
}

bool HttpEndpoint::origin_allowed(const std::string& origin) const {
    const std::string o = lower(strip_slash(trim(origin)));
    for (const auto& allowed : options_.cors_origins)
        if (allowed == o) return true;
    return false;
}

HttpResponse HttpEndpoint::finish(const HttpRequest& request, HttpResponse response) const {
    const std::string origin = request.header("Origin");
    if (!origin.empty() && origin_allowed(origin)) {
        response.set_header("Access-Control-Allow-Origin", origin);
        response.set_header("Vary", "Origin");
    }
    response.set_header("Cache-Control", response.header("Cache-Control").empty() ? "no-store" : response.header("Cache-Control"));
    response.set_header("X-Content-Type-Options", "nosniff");
    return response;
}

std::optional<HttpResponse> HttpEndpoint::precheck(const HttpRequest& request) const {
    // DNS-rebinding defence first: it applies to every path and method.
    if (!host_allowed(request.header("Host"))) {
        ++calls_denied_;
        return finish(request, plain_error(403, "HOST_NOT_ALLOWED",
                                           "Host header not allowed; add it with --allowed-hosts"));
    }
    const std::string origin = request.header("Origin");
    if (!origin.empty() && !origin_allowed(origin)) {
        ++calls_denied_;
        return finish(request, plain_error(403, "ORIGIN_NOT_ALLOWED",
                                           "Origin not allowed; add it with --cors-origin"));
    }
    if (request.path != options_.path)
        return finish(request, plain_error(404, "NOT_FOUND", "not found; the MCP endpoint is POST " + options_.path));

    if (request.method == "OPTIONS" && !origin.empty()) {  // CORS preflight of an allowed origin
        HttpResponse r = empty_response(204);
        r.set_header("Access-Control-Allow-Methods", "POST, OPTIONS");
        r.set_header("Access-Control-Allow-Headers",
                     "Content-Type, Authorization, Accept, Mcp-Method, Mcp-Name, MCP-Protocol-Version");
        r.set_header("Access-Control-Max-Age", "600");
        return finish(request, std::move(r));
    }
    if (request.method != "POST") {
        HttpResponse r = plain_error(405, "METHOD_NOT_ALLOWED", "only POST " + options_.path + " is supported");
        r.set_header("Allow", "POST");
        return finish(request, std::move(r));
    }
    std::string content_type = lower(trim(request.header("Content-Type")));
    content_type = trim(content_type.substr(0, content_type.find(';')));
    if (content_type != "application/json")
        return finish(request, plain_error(415, "UNSUPPORTED_MEDIA_TYPE", "Content-Type must be application/json"));

    const std::string length = trim(request.header("Content-Length"));
    bool too_big = request.body.size() > options_.max_body_bytes;
    if (!length.empty() && length.find_first_not_of("0123456789") == std::string::npos) {
        try {
            if (std::stoull(length) > options_.max_body_bytes) too_big = true;
        } catch (...) { too_big = true; }
    }
    if (too_big)
        return finish(request, plain_error(413, "PAYLOAD_TOO_LARGE",
                                           "request body exceeds " + std::to_string(options_.max_body_bytes) + " bytes"));
    return std::nullopt;
}

HttpEndpoint::PreAuth HttpEndpoint::preauthenticate(const HttpRequest& request) {
    PreAuth out;
    if (auto rejected = precheck(request)) {
        out.rejection = std::move(*rejected);
        return out;
    }

    // Authenticate BEFORE the body is read or parsed.
    DenyAllAuthenticator fallback;
    IAuthenticator& auth = authenticator_ ? *authenticator_ : static_cast<IAuthenticator&>(fallback);
    AuthRequest auth_request;
    auth_request.authorization = request.header("Authorization");
    auth_request.proxy_secret = request.header("X-Fairyfly-Proxy-Secret");
    auth_request.forwarded_for = request.header("X-Forwarded-For");
    auth_request.forwarded_proto = request.header("X-Forwarded-Proto");
    auth_request.peer_addr = request.peer_addr;
    AuthOutcome outcome;
    try {
        outcome = auth.authenticate(auth_request);
    } catch (const std::exception& e) {
        spdlog::error("authenticator failed: {}", e.what());
        outcome = AuthOutcome{};
        outcome.ok = false;
        outcome.http_status = 500;
        outcome.error_code = "AUTH_ERROR";
        outcome.message = "authentication failed";
    }
    if (!outcome.ok) {
        ++calls_denied_;
        const int status = outcome.http_status >= 400 ? outcome.http_status : 401;
        HttpResponse r = plain_error(status, outcome.error_code.empty() ? "AUTH_REQUIRED" : outcome.error_code,
                                     outcome.message.empty() ? "authentication required" : outcome.message);
        if (status == 401)
            r.set_header("WWW-Authenticate",
                         outcome.www_authenticate.empty() ? "Bearer realm=\"fairyfly\"" : outcome.www_authenticate);
        else if (!outcome.www_authenticate.empty())
            r.set_header("WWW-Authenticate", outcome.www_authenticate);
        out.rejection = finish(request, std::move(r));
        return out;
    }
    out.principal = outcome.principal;
    if (out.principal.remote_addr.empty()) out.principal.remote_addr = request.peer_addr;
    return out;
}

HttpResponse HttpEndpoint::handle_authenticated(const HttpRequest& request, const Principal& principal) {
    if (request.body.size() > options_.max_body_bytes)
        return finish(request, plain_error(413, "PAYLOAD_TOO_LARGE",
                                           "request body exceeds " + std::to_string(options_.max_body_bytes) + " bytes"));
    return finish(request, dispatch(request, principal));
}

HttpResponse HttpEndpoint::handle(const HttpRequest& request) {
    PreAuth pre = preauthenticate(request);
    if (pre.rejection) return std::move(*pre.rejection);
    return handle_authenticated(request, pre.principal);
}

HttpResponse HttpEndpoint::dispatch(const HttpRequest& request, const Principal& principal) {
    json parse_error;
    Message message = parse_message(request.body, parse_error);
    switch (message.kind) {
    case Message::Kind::Invalid:
        return json_response(400, parse_error.is_null() ? make_error(nullptr, kInvalidRequest, "Invalid request")
                                                        : parse_error);
    case Message::Kind::Response:
    case Message::Kind::Notification:
        return empty_response(202);
    case Message::Kind::Request:
        break;
    }

    const std::string& method = message.method;
    const json& params = message.params;

    // ---- Mcp-Method / Mcp-Name must match the body -------------------------------------------
    const std::string h_method = trim(request.header("Mcp-Method"));
    if (!h_method.empty() && h_method != method)
        return json_response(400, make_error(message.id, kHeaderMismatch,
                                             "Header mismatch: Mcp-Method '" + h_method + "' does not match body method '" + method + "'"));
    const auto h_name_it = request.headers.find("Mcp-Name");
    if (h_name_it != request.headers.end() && !trim(h_name_it->second).empty()) {
        std::string body_name;
        if (params.is_object() && params.contains("name") && params["name"].is_string())
            body_name = params["name"].get<std::string>();
        else if (params.is_object() && params.contains("uri") && params["uri"].is_string())
            body_name = params["uri"].get<std::string>();
        if (trim(h_name_it->second) != body_name)
            return json_response(400, make_error(message.id, kHeaderMismatch,
                                                 "Header mismatch: Mcp-Name '" + trim(h_name_it->second) +
                                                     "' does not match the request"));
    }

    // ---- protocol era / version ---------------------------------------------------------------
    const json meta_version = meta_value(params, "protocolVersion");
    const std::string pv = meta_version.is_string() ? meta_version.get<std::string>() : std::string();
    const std::string hv = trim(request.header("MCP-Protocol-Version"));
    ProtocolEra era = ProtocolEra::Legacy;
    if (method == "initialize") {
        era = ProtocolEra::Legacy;  // negotiated below; unsupported versions fall back to our newest legacy one
    } else {
        const std::string version = !pv.empty() ? pv : hv;
        if (!version.empty()) {
            const auto& supported = supported_versions();
            if (std::find(supported.begin(), supported.end(), version) == supported.end())
                return json_response(400, make_error(message.id, kUnsupportedVersion,
                                                     "Unsupported protocol version: " + version,
                                                     json{{"supported", supported_json()}, {"requested", version}}));
            if (is_stateless_version(version)) era = ProtocolEra::Stateless;
        } else if (method == "server/discover") {
            era = ProtocolEra::Stateless;
        }
    }
    const bool stateless = era == ProtocolEra::Stateless;
    auto reply = [&](json msg) {
        if (stateless) msg = decorate_stateless(std::move(msg), method);
        return json_response(200, msg);
    };

    // ---- methods answered without the main thread ---------------------------------------------
    if (method == "ping") return reply(make_result(message.id, json::object()));
    if (method == "server/discover") {
        json result{{"supportedVersions", supported_json()},
                    {"capabilities", http_capabilities()},
                    {"serverInfo", json{{"name", options_.server.name}, {"title", "fairyfly SAP GUI"},
                                        {"version", options_.server.version},
                                        {"description", "SAP GUI automation through the SAP GUI Scripting API"}}},
                    {"instructions", options_.server.instructions}};
        return reply(make_result(message.id, result));
    }
    if (method == "initialize") {
        if (!params.is_object() || !params.contains("protocolVersion") || !params["protocolVersion"].is_string())
            return reply(make_error(message.id, kInvalidParams, "initialize requires params.protocolVersion"));
        // No Mcp-Session-Id is minted: HTTP is stateless, every later request is served on its own.
        const std::vector<std::string> legacy(supported_versions().begin() + 1, supported_versions().end());
        json result = make_initialize_result(options_.server, negotiate_version(legacy, params["protocolVersion"].get<std::string>()));
        result["capabilities"] = http_capabilities();
        return reply(make_result(message.id, result));
    }

    const bool is_call = method == "tools/call";
    if (method != "tools/list" && !is_call) return reply(make_error(message.id, kMethodNotFound, "Method not found: " + method));

    // ---- tool methods: main thread via the executor -------------------------------------------
    const json client_info = meta_value(params, "clientInfo");
    const json progress_token = (params.is_object() && params.contains("_meta") && params["_meta"].is_object() &&
                                 params["_meta"].contains("progressToken"))
                                    ? params["_meta"]["progressToken"]
                                    : json();
    const std::string accept = lower(request.header("Accept"));
    const bool sse = options_.sse && is_call && accept.find("text/event-stream") != std::string::npos;

    auto waiter = std::make_shared<Waiter>();
    ExecJob job;
    job.id = message.id;
    job.timed = is_call;
    job.deliver = [waiter](const json& msg) { waiter->finish(msg); };
    if (is_call) {
        const json id = message.id;
        job.timeout_response = [id, stateless] {
            json msg = make_result(id, text_result("CALL_TIMEOUT: the SAP call is still running; retry after it completes", true));
            return stateless ? decorate_stateless(std::move(msg), "tools/call") : msg;
        };
    }
    Pending pending{message.id, method, params};
    const bool want_progress = sse && !progress_token.is_null();
    job.run = [this, pending, principal, era, stateless, client_info, progress_token, want_progress,
               waiter](CallState& state) -> json {
        json out;
        if (pending.method == "tools/list") {
            out = tools_list_message(provider_, pending, true);
        } else {
            try {
                provider_.set_client_info(client_info.is_object() ? client_info : json::object());
            } catch (const std::exception& e) {
                spdlog::warn("set_client_info failed: {}", e.what());
            }
            CallContext ctx;
            ctx.principal = principal;
            ctx.era = era;
            ctx.http = true;
            ctx.cancelled = [&state] { return state.cancelled.load(); };
            auto last = std::make_shared<double>(0.0);
            if (want_progress) {
                waiter->push_frame(progress_frame(progress_token, 0.0, std::nullopt, "started"));
                ctx.report_progress = [waiter, progress_token, last](double progress, std::optional<double> total,
                                                                      const std::string& text) {
                    if (progress <= *last) return;
                    *last = progress;
                    waiter->push_frame(progress_frame(progress_token, progress, total, text));
                };
            }
            out = call_tool_message(provider_, pending, std::move(ctx));
            if (want_progress) waiter->push_frame(progress_frame(progress_token, *last + 1.0, std::nullopt, "finished"));
        }
        return stateless ? decorate_stateless(std::move(out), pending.method) : out;
    };

    std::shared_ptr<CallState> state;
    switch (executor_.submit(std::move(job), &state)) {
    case SubmitResult::Queued:
        break;
    case SubmitResult::Busy:
        return reply(make_result(message.id, text_result("SERVER_BUSY: a previous SAP call is still running; retry shortly", true)));
    case SubmitResult::QueueFull: {
        HttpResponse r = json_response(503, make_error(message.id, kServerBusy, "server busy", json{{"retry", true}}));
        r.set_header("Retry-After", "1");
        return r;
    }
    }
    if (is_call) ++calls_total_;

    const auto poll = std::chrono::milliseconds(std::max(1, options_.poll_ms));
    auto shutting_down = [&] {
        HttpResponse r = json_response(503, make_error(message.id, kServerBusy, "server shutting down", json{{"retry", true}}));
        return r;
    };

    if (!sse) {
        std::unique_lock<std::mutex> lock(waiter->m);
        while (!waiter->done) {
            waiter->cv.wait_for(lock, poll);
            if (!waiter->done && executor_.stopping()) break;
        }
        if (!waiter->done || waiter->final_message.is_null()) {
            lock.unlock();
            executor_.cancel(state);
            return shutting_down();
        }
        return json_response(200, waiter->final_message);
    }

    // ---- SSE ----------------------------------------------------------------------------------
    HttpResponse r;
    r.status = 200;
    r.set_header("Content-Type", "text/event-stream");
    r.set_header("Cache-Control", "no-cache");
    r.set_header("X-Accel-Buffering", "no");
    r.set_header("Connection", "keep-alive");
    const int keepalive_ms = options_.keepalive_ms;
    CallExecutor* executor = &executor_;
    r.stream = [waiter, state, executor, poll, keepalive_ms](SseSink& sink) {
        auto last_write = std::chrono::steady_clock::now();
        auto emit = [&](const std::string& frame) {
            if (!sink.write(frame)) return false;
            last_write = std::chrono::steady_clock::now();
            return true;
        };
        while (true) {
            std::vector<std::string> frames;
            json final_message;
            bool done = false;
            {
                std::unique_lock<std::mutex> lock(waiter->m);
                if (waiter->frames.empty() && !waiter->done) waiter->cv.wait_for(lock, poll);
                while (!waiter->frames.empty()) {
                    frames.push_back(std::move(waiter->frames.front()));
                    waiter->frames.pop_front();
                }
                done = waiter->done;
                final_message = waiter->final_message;
            }
            for (const auto& f : frames)
                if (!emit(f)) {
                    executor->cancel(state);
                    return;
                }
            if (done) {
                if (final_message.is_null())
                    final_message = make_error(state->id, kServerBusy, "server shutting down", json{{"retry", true}});
                emit(format_sse_event("message", dump(final_message)));
                return;
            }
            if (!sink.connected() || executor->stopping()) {
                executor->cancel(state);
                return;
            }
            if (keepalive_ms > 0 && std::chrono::steady_clock::now() - last_write >= std::chrono::milliseconds(keepalive_ms))
                if (!emit(format_sse_comment("keep-alive"))) {
                    executor->cancel(state);
                    return;
                }
        }
    };
    return r;
}

} // namespace fairyfly::mcp
