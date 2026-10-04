#include "include/mcp/http_endpoint.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>

#include <spdlog/spdlog.h>

#include "include/auth/ip.h"
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

constexpr const char* kMetaPrefix = "io.modelcontextprotocol/";

/// params._meta[key]: the "io.modelcontextprotocol/" prefixed spelling (the spec's) wins over the plain one.
json meta_value(const json& params, const char* key) {
    if (!params.is_object() || !params.contains("_meta") || !params["_meta"].is_object()) return json();
    const json& meta = params["_meta"];
    const std::string prefixed = std::string(kMetaPrefix) + key;
    if (meta.contains(prefixed)) return meta[prefixed];
    if (meta.contains(key)) return meta[key];
    return json();
}

/// True when _meta carries both spellings of `key` with different values.
bool meta_conflict(const json& params, const char* key) {
    if (!params.is_object() || !params.contains("_meta") || !params["_meta"].is_object()) return false;
    const json& meta = params["_meta"];
    const std::string prefixed = std::string(kMetaPrefix) + key;
    return meta.contains(prefixed) && meta.contains(key) && meta[prefixed] != meta[key];
}

/// Header value for an error message: control characters escaped, at most 100 characters.
std::string shown(const std::string& value) {
    std::string out;
    std::size_t count = 0;
    for (unsigned char c : value) {
        if (count >= 100) {
            out += "...";
            break;
        }
        if (c < 0x20 || c == 0x7f) {
            static const char* hex = "0123456789abcdef";
            out += "\\x";
            out += hex[c >> 4];
            out += hex[c & 15];
        } else {
            out += static_cast<char>(c);
        }
        ++count;
    }
    return out;
}

/// Strict standard base64 (padded, no whitespace). nullopt when malformed.
std::optional<std::string> base64_decode(const std::string& in) {
    if (in.size() % 4 != 0) return std::nullopt;
    std::string out;
    auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '+') return 62;
        if (c == '/') return 63;
        return -1;
    };
    for (std::size_t i = 0; i < in.size(); i += 4) {
        const bool last = i + 4 == in.size();
        int pad = 0;
        int v[4];
        for (int k = 0; k < 4; ++k) {
            const char c = in[i + k];
            if (c == '=') {
                if (!last || k < 2) return std::nullopt;
                ++pad;
                v[k] = 0;
            } else {
                if (pad > 0) return std::nullopt;
                v[k] = val(c);
                if (v[k] < 0) return std::nullopt;
            }
        }
        const unsigned n = (v[0] << 18) | (v[1] << 12) | (v[2] << 6) | v[3];
        out += static_cast<char>((n >> 16) & 0xff);
        if (pad < 2) out += static_cast<char>((n >> 8) & 0xff);
        if (pad < 1) out += static_cast<char>(n & 0xff);
    }
    return out;
}

/// Decodes the spec's "=?base64?<base64 of UTF-8>?=" header sentinel; any other value is returned unchanged.
/// nullopt = starts like a sentinel but is malformed.
std::optional<std::string> decode_header_value(const std::string& value) {
    static const std::string open = "=?base64?";
    static const std::string close = "?=";
    if (value.compare(0, open.size(), open) != 0) return value;
    if (value.size() < open.size() + close.size() || value.compare(value.size() - close.size(), close.size(), close) != 0)
        return std::nullopt;
    return base64_decode(value.substr(open.size(), value.size() - open.size() - close.size()));
}

HttpResponse header_error(const json& id, const std::string& message) {
    return json_response(400, make_error(id, kHeaderMismatch, message));
}

bool is_stateless_version(const std::string& v) { return v == kStatelessVersion; }

json supported_json() {
    json a = json::array();
    for (const auto& v : HttpEndpoint::supported_versions()) a.push_back(v);
    return a;
}

/// Negotiates against ALL served versions; an unsupported request falls back to the newest legacy version.
std::string negotiate_http_version(const std::string& requested) {
    const auto& all = HttpEndpoint::supported_versions();
    if (std::find(all.begin(), all.end(), requested) != all.end()) return requested;
    return all.size() > 1 ? all[1] : all.front();
}

json http_capabilities() { return json{{"tools", json{{"listChanged", false}}}}; }

json decorate_stateless(json message, const std::string& method, const json& server_info) {
    if (message.is_object() && message.contains("result") && message["result"].is_object()) {
        json& result = message["result"];
        result["resultType"] = "complete";
        if (method == "tools/list") {
            result["ttlMs"] = 30000;
            result["cacheScope"] = "private";
        }
        if (!result.contains("_meta") || !result["_meta"].is_object()) result["_meta"] = json::object();
        result["_meta"][std::string(kMetaPrefix) + "serverInfo"] = server_info;
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

bool accept_prefers_sse(std::string_view accept_header, bool has_progress_token) {
    // best q per specificity: [0] exact type, [1] type/*, [2] */*; -1 = not listed
    double sse[3] = {-1, -1, -1};
    double json_q[3] = {-1, -1, -1};
    const std::string header = lower(std::string(accept_header));
    size_t pos = 0;
    while (pos <= header.size()) {
        size_t comma = header.find(',', pos);
        if (comma == std::string::npos) comma = header.size();
        std::string part = header.substr(pos, comma - pos);
        pos = comma + 1;
        double q = 1.0;
        std::string media = part;
        const size_t semi = part.find(';');
        if (semi != std::string::npos) {
            media = part.substr(0, semi);
            const size_t qpos = part.find("q=", semi);
            if (qpos != std::string::npos) {
                try {
                    q = std::stod(part.substr(qpos + 2));
                } catch (...) {
                    q = 1.0;
                }
            }
        }
        const auto trim = [](std::string& t) {
            while (!t.empty() && std::isspace(static_cast<unsigned char>(t.front()))) t.erase(t.begin());
            while (!t.empty() && std::isspace(static_cast<unsigned char>(t.back()))) t.pop_back();
        };
        trim(media);
        if (media.empty()) continue;
        if (media == "text/event-stream") sse[0] = std::max(sse[0], q);
        else if (media == "application/json") json_q[0] = std::max(json_q[0], q);
        else if (media == "application/*") json_q[1] = std::max(json_q[1], q);
        else if (media == "*/*") json_q[2] = std::max(json_q[2], q);
    }
    const auto effective = [](const double (&v)[3]) {
        for (double q : v)
            if (q >= 0) return q;  // most specific listing wins
        return -1.0;
    };
    const double q_sse = sse[0];  // only an explicit listing enables SSE; wildcards never do
    const double q_json = effective(json_q);
    if (q_sse <= 0) return false;
    if (has_progress_token) return true;
    if (q_json <= 0) return true;
    return q_sse > q_json;
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

bool same_security_principal(const Principal& a, const Principal& b) {
    return a.id == b.id && a.name == b.name && a.scopes == b.scopes &&
           a.all_scopes == b.all_scopes && a.sap_systems == b.sap_systems &&
           a.sap_identities == b.sap_identities &&
           a.tcodes == b.tcodes && a.connections == b.connections &&
           a.rate_per_minute == b.rate_per_minute && a.rate_families == b.rate_families &&
           a.read_only == b.read_only && a.allow_navigation == b.allow_navigation &&
           a.allow_selection_input == b.allow_selection_input && a.authenticated == b.authenticated;
}

void HttpEndpoint::set_session_router(SessionExecutorPool* pool, SessionRouter router,
                                      SessionRouter recheck_router) {
    session_pool_ = pool;
    session_router_ = std::move(router);
    session_recheck_router_ = recheck_router ? std::move(recheck_router) : session_router_;
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
    // Server-level client allow-list first (before Host/Origin/auth/body). A
    // configured strict list also covers loopback on shared Windows hosts.
    if ((options_.allow_ip_include_loopback || !options_.allow_ip.empty()) &&
        (options_.allow_ip_include_loopback || !auth::ip_is_loopback(request.peer_addr)) &&
        !auth::ip_allowed(request.peer_addr, options_.allow_ip)) {
        ++calls_denied_;
        return finish(request, plain_error(403, "ADDRESS_NOT_ALLOWED",
                                           "this address is not allowed to use this server"));
    }
    // DNS-rebinding defence: it applies to every path and method.
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

    // ---- _meta spellings: prefixed (spec) and plain may both be present only when they agree --------
    for (const char* key : {"protocolVersion", "clientInfo", "clientCapabilities", "logLevel"})
        if (meta_conflict(params, key))
            return json_response(400, make_error(message.id, kInvalidParams,
                                                 std::string("Invalid params: _meta key '") + key + "' is given twice with different values"));

    // ---- classification: modern (2026-07-28 stateless) or legacy ------------------------------------
    // Modern: params._meta protocolVersion (prefixed or plain key) >= 2026-07-28, or method server/discover, or
    // the MCP-Protocol-Version header says 2026-07-28. Everything else is legacy and keeps the lenient behaviour.
    const json meta_version = meta_value(params, "protocolVersion");
    const std::string pv = meta_version.is_string() ? meta_version.get<std::string>() : std::string();
    const std::string hv = trim(request.header("MCP-Protocol-Version"));
    const bool modern = (!pv.empty() && pv >= std::string(kStatelessVersion)) || method == "server/discover" ||
                        hv == kStatelessVersion;
    const std::string version = !pv.empty() ? pv : hv;

    // MCP-Protocol-Version header against the body's _meta version: in EVERY era, when both are present they must be
    // byte-equal (after trimming). Checked before the version is validated, so a supported body version cannot hide behind a
    // different (e.g. unsupported) header. An unsupported header alone keeps -32022 below.
    if (!hv.empty() && !pv.empty() && pv != hv)
        return header_error(message.id, "Header mismatch: MCP-Protocol-Version header value '" + shown(hv) +
                                            "' does not match body value '" + shown(pv) + "'");

    // Unsupported version: 400 + -32022 listing what is served (modern probes read this to recognise the server).
    if (method != "initialize" && !version.empty()) {
        const auto& supported = supported_versions();
        if (std::find(supported.begin(), supported.end(), version) == supported.end())
            return json_response(400, make_error(message.id, kUnsupportedVersion, "Unsupported protocol version: " + version,
                                                 json{{"supported", supported_json()}, {"requested", version}}));
    }

    // ---- standard headers of a modern request: checked before any provider call --------------------
    // Mcp-Method / Mcp-Name against the body: REQUIRED for a modern request, and in EVERY era a header that is present
    // must agree with the body (a legacy request is executed all the same). Absent headers stay accepted on legacy requests.
    const auto check_method_name_headers = [&](bool required) -> std::optional<HttpResponse> {
        const std::string h_method = trim(request.header("Mcp-Method"));
        if (h_method.empty()) {
            if (required) return header_error(message.id, "Header missing: Mcp-Method");
        } else if (h_method != method) {
            return header_error(message.id, "Header mismatch: Mcp-Method header value '" + shown(h_method) +
                                                "' does not match body value '" + shown(method) + "'");
        }
        const bool named = method == "tools/call" || method == "resources/read" || method == "prompts/get";
        if (named) {
            std::string body_name;
            bool have_body_name = false;
            const char* key = method == "resources/read" ? "uri" : "name";
            if (params.is_object() && params.contains(key) && params[key].is_string()) {
                body_name = params[key].get<std::string>();
                have_body_name = true;
            }
            const std::string raw = trim(request.header("Mcp-Name"));
            if (raw.empty()) {
                if (required) return header_error(message.id, "Header missing: Mcp-Name");
                return std::nullopt;
            }
            const auto decoded = decode_header_value(raw);
            if (!decoded)
                return header_error(message.id, "Header mismatch: Mcp-Name header value '" + shown(raw) +
                                                    "' is not valid =?base64?...?= encoding");
            // A body without a string name is left to the tool handler (INVALID_PARAMS).
            if (have_body_name && *decoded != body_name)
                return header_error(message.id, "Header mismatch: Mcp-Name header value '" + shown(*decoded) +
                                                    "' does not match body value '" + shown(body_name) + "'");
        }
        return std::nullopt;
    };
    if (modern) {
        if (hv.empty()) return header_error(message.id, "Header missing: MCP-Protocol-Version");
        if (auto refused = check_method_name_headers(true)) return std::move(*refused);
    } else {
        if (auto refused = check_method_name_headers(false)) return std::move(*refused);
    }

    // ---- protocol era ------------------------------------------------------------------------------
    ProtocolEra era = ProtocolEra::Legacy;
    if (method == "initialize") {
        // The era follows the negotiated version: a client asking for the stateless version gets a stateless
        // answer, everything else (including versions we do not know) the legacy one.
        const std::string requested = params.is_object() && params.contains("protocolVersion") && params["protocolVersion"].is_string()
                                          ? params["protocolVersion"].get<std::string>() : std::string();
        era = (negotiate_http_version(requested) == kStatelessVersion || modern) ? ProtocolEra::Stateless : ProtocolEra::Legacy;
    } else if (modern || is_stateless_version(version)) {
        era = ProtocolEra::Stateless;
    }
    const bool stateless = era == ProtocolEra::Stateless;
    const json server_info{{"name", options_.server.name}, {"version", options_.server.version}};
    auto reply = [&](json msg) {
        if (stateless) msg = decorate_stateless(std::move(msg), method, server_info);
        return json_response(200, msg);
    };

    // Modern era: ping and logging/setLevel were removed; any unknown method is HTTP 404.
    if (modern && (method == "ping" || method == "logging/setLevel"))
        return json_response(404, make_error(message.id, kMethodNotFound, "Method not found: " + method));

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
        json result = make_initialize_result(options_.server, negotiate_http_version(params["protocolVersion"].get<std::string>()));
        result["capabilities"] = http_capabilities();
        return reply(make_result(message.id, result));
    }

    const bool is_call = method == "tools/call";
    if (method != "tools/list" && !is_call) {
        if (modern) return json_response(404, make_error(message.id, kMethodNotFound, "Method not found: " + method));
        return reply(make_error(message.id, kMethodNotFound, "Method not found: " + method));
    }

    // ---- tool methods: main thread via the executor -------------------------------------------
    const json client_info = meta_value(params, "clientInfo");
    const json progress_token = (params.is_object() && params.contains("_meta") && params["_meta"].is_object() &&
                                 params["_meta"].contains("progressToken"))
                                    ? params["_meta"]["progressToken"]
                                    : json();
    const bool sse = options_.sse && is_call && accept_prefers_sse(request.header("Accept"), !progress_token.is_null());

    SessionRoute session_route;
    const AuthRequest execution_auth{request.header("Authorization"), request.peer_addr};
    Principal admission_principal = principal;
    if (is_call && session_pool_ && session_router_ && params.is_object() &&
        params.contains("name") && params["name"].is_string()) {
        AuthOutcome refreshed;
        try {
            if (authenticator_) {
                authenticator_->invalidate_cache();
                refreshed = authenticator_->authenticate(execution_auth);
            }
        } catch (...) {
            refreshed.ok = false;
            refreshed.error_code = "AUTH_UNAVAILABLE";
        }
        if (!refreshed.ok || !same_security_principal(refreshed.principal, principal)) {
            const std::string code = refreshed.error_code.empty() ? "TOKEN_CHANGED" : refreshed.error_code;
            return reply(make_error(message.id, kInvalidParams, "authorization changed before route admission",
                                    json{{"code", code}}));
        }
        admission_principal = std::move(refreshed.principal);
        const std::string tool = params["name"].get<std::string>();
        try {
            session_route = session_router_(admission_principal, tool,
                                            params.value("arguments", json::object()));
        } catch (...) {
            session_route.handled = true;
            session_route.error_code = "SESSION_ROUTE_UNAVAILABLE";
            session_route.error_message = "the SAP session could not be routed";
        }
        const bool global_control = session_route.global_control ||
                                    tool == "gui_session_list" || tool == "gui_connection_list" ||
                                    tool == "gui_credentials_list" || tool == "gui_doctor" ||
                                    tool == "gui_session_lease" || tool == "gui_session_attach" ||
                                    tool == "gui_session_login" || tool == "gui_session_launch";
        if (!session_route.error_code.empty() ||
            (session_route.handled && (session_route.identity.empty() || !session_route.provider)) ||
            (!session_route.handled && !global_control)) {
            const std::string code = session_route.error_code.empty() ? "SESSION_ROUTE_UNAVAILABLE" : session_route.error_code;
            const std::string effective_code = !session_route.handled && !global_control && session_route.error_code.empty()
                                                   ? "SESSION_ROUTE_REQUIRED" : code;
            const std::string reason = session_route.error_message.empty() ? "the SAP session could not be routed"
                                                                    : session_route.error_message;
            return reply(make_error(message.id, kInvalidParams, reason, json{{"code", effective_code}}));
        }
    }
    const bool routed = session_route.handled;
    const std::string route_identity = session_route.identity;
    const std::string route_lane_key = session_route.lane_key.empty() ? route_identity : session_route.lane_key;
    std::shared_ptr<ToolProvider> route_provider = std::move(session_route.provider);

    auto waiter = std::make_shared<Waiter>();
    ExecJob job;
    job.id = message.id;
    job.timed = is_call;
    job.deliver = [waiter](const json& msg) { waiter->finish(msg); };
    if (is_call) {
        const json id = message.id;
        if (params.is_object() && params.contains("name") && params["name"].is_string()) job.tool = params["name"].get<std::string>();
        job.timeout_response = [id, stateless, server_info](const CallInfo& info) {
            json msg = make_result(id, busy_call_result("CALL_TIMEOUT", info));
            return stateless ? decorate_stateless(std::move(msg), "tools/call", server_info) : msg;
        };
    }
    Pending pending{message.id, method, params};
    const bool want_progress = sse && !progress_token.is_null();
    job.run = [this, pending, principal = admission_principal, execution_auth, era, stateless, server_info, client_info, progress_token, want_progress,
               route_provider, route_identity, route_lane_key, routed,
               waiter](CallState& state) -> json {
        AuthOutcome refreshed;
        try {
            if (authenticator_) {
                authenticator_->invalidate_cache();
                refreshed = authenticator_->authenticate(execution_auth);
            }
        } catch (...) {
            refreshed.ok = false;
            refreshed.error_code = "AUTH_UNAVAILABLE";
        }
        if (!refreshed.ok || !same_security_principal(refreshed.principal, principal)) {
            const std::string reason = refreshed.error_code.empty() ? "TOKEN_CHANGED" : refreshed.error_code;
            json denied = make_error(pending.id, kInvalidParams, "authorization changed before execution",
                                     json{{"code", reason}});
            return stateless ? decorate_stateless(std::move(denied), pending.method, server_info) : denied;
        }
        const Principal& current_principal = refreshed.principal;
        if (routed) {
            bool valid_route = false;
            try {
                const SessionRoute current = session_recheck_router_(current_principal,
                    pending.params.at("name").get<std::string>(),
                    pending.params.value("arguments", json::object()));
                valid_route = current.handled && current.error_code.empty() &&
                              current.identity == route_identity &&
                              (current.lane_key.empty() ? current.identity : current.lane_key) == route_lane_key &&
                              current.provider.get() == route_provider.get();
            } catch (...) {
                valid_route = false;
            }
            if (!valid_route) {
                json denied = make_error(pending.id, kInvalidParams, "SAP session routing changed before execution",
                                         json{{"code", "SESSION_ROUTE_CHANGED"}});
                return stateless ? decorate_stateless(std::move(denied), pending.method, server_info) : denied;
            }
        }
        ToolProvider& selected_provider = route_provider ? *route_provider : provider_;
        json out;
        if (pending.method == "tools/list") {
            out = tools_list_message(selected_provider, pending, true, &current_principal);
        } else {
            try {
                selected_provider.set_client_info(client_info.is_object() ? client_info : json::object());
            } catch (const std::exception& e) {
                spdlog::warn("set_client_info failed: {}", e.what());
            }
            CallContext ctx;
            ctx.principal = current_principal;
            ctx.era = era;
            ctx.http = true;
            ctx.cancelled = [&state] { return state.cancelled.load(); };
            ctx.reauthorize = [this, execution_auth, bound = current_principal]() {
                if (!authenticator_) return false;
                try {
                    authenticator_->invalidate_cache();
                    const AuthOutcome latest = authenticator_->authenticate(execution_auth);
                    return latest.ok && same_security_principal(latest.principal, bound);
                } catch (...) {
                    return false;
                }
            };
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
            out = call_tool_message(selected_provider, pending, std::move(ctx));
            if (want_progress) waiter->push_frame(progress_frame(progress_token, *last + 1.0, std::nullopt, "finished"));
        }
        return stateless ? decorate_stateless(std::move(out), pending.method, server_info) : out;
    };

    std::shared_ptr<CallState> state;
    const SubmitResult submission = routed ? session_pool_->submit(route_lane_key, std::move(job), &state)
                                           : executor_.submit(std::move(job), &state);
    switch (submission) {
    case SubmitResult::Queued:
        break;
    case SubmitResult::Busy:
        return reply(make_result(message.id, busy_call_result("SERVER_BUSY",
            routed ? session_pool_->running_info(route_lane_key).value_or(CallInfo{})
                   : executor_.running_info().value_or(CallInfo{}))));
    case SubmitResult::QueueFull: {
        HttpResponse r = json_response(503, make_error(message.id, kServerBusy, "server busy", json{{"retry", true}}));
        r.set_header("Retry-After", "1");
        return r;
    }
    }
    if (is_call) ++calls_total_;

    const auto poll = std::chrono::milliseconds(std::max(1, options_.poll_ms));
    const auto route_cancel = [this, routed, route_lane_key, state] {
        if (routed) session_pool_->cancel(route_lane_key, state);
        else executor_.cancel(state);
    };
    const auto route_stopping = [this, routed] {
        return executor_.stopping() || (routed && session_pool_->stopping());
    };
    auto shutting_down = [&] {
        HttpResponse r = json_response(503, make_error(message.id, kServerBusy, "server shutting down", json{{"retry", true}}));
        return r;
    };

    if (!sse) {
        std::unique_lock<std::mutex> lock(waiter->m);
        while (!waiter->done) {
            waiter->cv.wait_for(lock, poll);
            if (!waiter->done && request.connected && !request.connected()) {
                lock.unlock();
                route_cancel();
                return json_response(499, make_error(message.id, kServerBusy, "client disconnected"));
            }
            if (!waiter->done && route_stopping()) break;
        }
        if (!waiter->done || waiter->final_message.is_null()) {
            lock.unlock();
            route_cancel();
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
    r.stream = [waiter, state, route_cancel, route_stopping, poll, keepalive_ms](SseSink& sink) {
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
                    route_cancel();
                    return;
                }
            if (done) {
                if (final_message.is_null())
                    final_message = make_error(state->id, kServerBusy, "server shutting down", json{{"retry", true}});
                emit(format_sse_event("message", dump(final_message)));
                return;
            }
            if (!sink.connected() || route_stopping()) {
                route_cancel();
                return;
            }
            if (keepalive_ms > 0 && std::chrono::steady_clock::now() - last_write >= std::chrono::milliseconds(keepalive_ms))
                if (!emit(format_sse_comment("keep-alive"))) {
                    route_cancel();
                    return;
                }
        }
    };
    return r;
}

} // namespace fairyfly::mcp
