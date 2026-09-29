// Remote MCP protocol core: HttpEndpoint (pure, no sockets), SSE framing, CallExecutor and a loopback
// smoke test of the cpp-httplib adapter. No SAP, no COM; every wait is bounded.
#include <catch2/catch_test_macros.hpp>

#include <httplib.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "include/mcp/authenticators.h"
#include "include/mcp/call_executor.h"
#include "include/mcp/http_endpoint.h"
#include "include/mcp/http_server.h"
#include "include/mcp/json_rpc.h"
#include "include/mcp/protocol_session.h"

using namespace fairyfly::mcp;
using namespace std::chrono_literals;

namespace {

/// Fake tool backend. Records what reached it (principal, era, http, client info).
class FakeProvider : public ToolProvider {
public:
    std::vector<ToolDef> list_tools() const override {
        std::vector<ToolDef> out;
        for (const char* n : {"gui_screen_read", "gui_a_tool", "gui_slow", "gui_wait_cancel", "gui_session_list"}) {
            ToolDef d;
            d.name = n;
            d.title = n;
            d.description = "fake";
            d.input_schema = json{{"type", "object"}};
            out.push_back(d);
        }
        return out;  // deliberately NOT sorted
    }
    bool has_tool(const std::string& name) const override {
        return name == "gui_element_fill" || ToolProvider::has_tool(name);  // hidden write tool
    }
    ToolResult call_tool(const std::string& name, const json& args, const CallContext& ctx) override {
        {
            std::lock_guard<std::mutex> lock(m);
            last_name = name;
            last_principal = ctx.principal;
            last_era = ctx.era;
            last_http = ctx.http;
            last_args = args;
            last_request_id = ctx.request_id;
            ++calls;
        }
        ToolResult r;
        if (name == "gui_element_fill") {
            r.is_error = true;
            r.content = json::array({json{{"type", "text"}, {"text", "ERROR TOOL_UNAVAILABLE_READ_ONLY: hidden"}}});
            return r;
        }
        if (name == "gui_slow") {
            const auto until = std::chrono::steady_clock::now() + std::chrono::milliseconds(args.value("ms", 300));
            while (std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(5ms);
        } else if (name == "gui_wait_cancel") {
            const auto until = std::chrono::steady_clock::now() + 3s;
            while (std::chrono::steady_clock::now() < until) {
                if (ctx.cancelled && ctx.cancelled()) {
                    saw_cancel = true;
                    break;
                }
                std::this_thread::sleep_for(5ms);
            }
        } else if (name == "gui_a_tool" && ctx.report_progress) {
            ctx.report_progress(0.5, 1.0, "half");
        }
        r.content = json::array({json{{"type", "text"}, {"text", "ok:" + name}}});
        r.structured = json{{"tool", name}};
        return r;
    }
    void set_client_info(const json& info) override {
        std::lock_guard<std::mutex> lock(m);
        client_info = info;
    }

    std::mutex m;
    std::string last_name;
    Principal last_principal;
    ProtocolEra last_era = ProtocolEra::Legacy;
    bool last_http = false;
    json last_args, last_request_id, client_info;
    int calls = 0;
    std::atomic<bool> saw_cancel{false};
};

class TokenAuth : public IAuthenticator {
public:
    AuthOutcome authenticate(const AuthRequest& r) override {
        last = r;
        AuthOutcome o;
        if (r.authorization == "Bearer good") {
            o.ok = true;
            o.http_status = 200;
            o.principal.name = "alice";
            o.principal.all_scopes = false;
            o.principal.scopes = {"screen"};
            o.principal.remote_addr = r.forwarded_for.empty() ? r.peer_addr : r.forwarded_for;
            o.principal.authenticated = true;
            return o;
        }
        o.ok = false;
        o.http_status = r.authorization.empty() ? 401 : 403;
        o.error_code = r.authorization.empty() ? "AUTH_REQUIRED" : "TOKEN_INVALID";
        o.message = "no";
        o.www_authenticate = "Bearer realm=\"test\"";
        return o;
    }
    AuthRequest last;
};

/// Runs the CallExecutor loop on a helper thread (stands in for the COM main thread).
struct Fixture {
    explicit Fixture(int call_timeout_ms = 5000, std::size_t queue = 16, HttpEndpointOptions opts = {})
        : exec(queue, call_timeout_ms), endpoint(prepare(std::move(opts)), exec, provider, &auth) {
        loop = std::thread([this] { exec.run(); });
    }
    ~Fixture() {
        exec.request_stop();
        if (loop.joinable()) loop.join();
    }
    static HttpEndpointOptions prepare(HttpEndpointOptions o) {
        o.server.name = "fairyfly";
        o.server.version = "9.9.9";
        o.server.instructions = "be careful";
        if (o.poll_ms == 100) o.poll_ms = 10;
        return o;
    }

    HttpRequest post(const json& body, bool auth_header = true) const {
        HttpRequest r;
        r.method = "POST";
        r.path = "/mcp";
        r.headers["Host"] = "127.0.0.1:8383";
        r.headers["Content-Type"] = "application/json";
        if (auth_header) r.headers["Authorization"] = "Bearer good";
        r.body = body.dump();
        r.peer_addr = "127.0.0.1";
        return r;
    }
    static json stateless_meta() {
        return json{{"protocolVersion", "2026-07-28"},
                    {"clientInfo", json{{"name", "tester"}, {"version", "1"}}},
                    {"capabilities", json::object()}};
    }
    static json rpc(const std::string& method, json params = json::object(), int id = 1) {
        return json{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}};
    }

    FakeProvider provider;
    TokenAuth auth;
    CallExecutor exec;
    HttpEndpoint endpoint;
    std::thread loop;
};

json body_of(const HttpResponse& r) { return json::parse(r.body); }

class VectorSink : public SseSink {
public:
    bool write(std::string_view frame) override {
        std::lock_guard<std::mutex> lock(m);
        if (drop_after >= 0 && static_cast<int>(frames.size()) >= drop_after) return false;
        frames.emplace_back(frame);
        return true;
    }
    bool connected() const override { return !gone.load(); }
    std::string all() {
        std::lock_guard<std::mutex> lock(m);
        std::string s;
        for (const auto& f : frames) s += f;
        return s;
    }
    std::mutex m;
    std::vector<std::string> frames;
    std::atomic<bool> gone{false};
    int drop_after = -1;
};

} // namespace

// ---- SSE framing (pure) -------------------------------------------------------------------------

TEST_CASE("SSE frame formatting", "[mcp][http][sse]") {
    CHECK(format_sse_event("message", "{\"a\":1}") == "event: message\ndata: {\"a\":1}\n\n");
    CHECK(format_sse_event("message", "l1\nl2", "7") == "id: 7\nevent: message\ndata: l1\ndata: l2\n\n");
    CHECK(format_sse_event("", "x") == "data: x\n\n");
    CHECK(format_sse_comment("keep-alive") == ": keep-alive\n\n");
}

// ---- routing / limits ---------------------------------------------------------------------------

TEST_CASE("HTTP routing, methods and limits", "[mcp][http]") {
    Fixture f;
    SECTION("unknown path is 404") {
        auto req = f.post(Fixture::rpc("ping"));
        req.path = "/other";
        CHECK(f.endpoint.handle(req).status == 404);
    }
    SECTION("GET and DELETE are 405 with Allow: POST") {
        for (const char* method : {"GET", "DELETE", "PUT"}) {
            auto req = f.post(Fixture::rpc("ping"));
            req.method = method;
            auto res = f.endpoint.handle(req);
            CHECK(res.status == 405);
            CHECK(res.header("Allow") == "POST");
        }
    }
    SECTION("content type must be application/json") {
        auto req = f.post(Fixture::rpc("ping"));
        req.headers["Content-Type"] = "text/plain";
        CHECK(f.endpoint.handle(req).status == 415);
        req.headers["Content-Type"] = "Application/JSON; charset=utf-8";
        CHECK(f.endpoint.handle(req).status == 200);
    }
    SECTION("body over 1 MiB is 413") {
        auto req = f.post(Fixture::rpc("ping"));
        req.body = std::string(1024 * 1024 + 1, ' ');
        CHECK(f.endpoint.handle(req).status == 413);
        auto by_header = f.post(Fixture::rpc("ping"));
        by_header.headers["Content-Length"] = "2000000";
        CHECK(f.endpoint.handle(by_header).status == 413);
    }
    SECTION("malformed JSON and batches are 400") {
        auto req = f.post(json());
        req.body = "{not json";
        auto res = f.endpoint.handle(req);
        CHECK(res.status == 400);
        CHECK(body_of(res)["error"]["code"] == kParseError);
        req.body = "[]";
        CHECK(f.endpoint.handle(req).status == 400);
    }
    SECTION("notifications and client responses are 202 with an empty body") {
        json note{{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}};
        auto res = f.endpoint.handle(f.post(note));
        CHECK(res.status == 202);
        CHECK(res.body.empty());
        json response{{"jsonrpc", "2.0"}, {"id", 5}, {"result", json::object()}};
        CHECK(f.endpoint.handle(f.post(response)).status == 202);
    }
    SECTION("unknown method is -32601 and ping works") {
        auto res = f.endpoint.handle(f.post(Fixture::rpc("resources/list")));
        CHECK(res.status == 200);
        CHECK(body_of(res)["error"]["code"] == kMethodNotFound);
        CHECK(body_of(f.endpoint.handle(f.post(Fixture::rpc("ping"))))["result"] == json::object());
    }
    SECTION("logging/setLevel is not exposed remotely") {
        auto res = f.endpoint.handle(f.post(Fixture::rpc("logging/setLevel", json{{"level", "debug"}})));
        CHECK(body_of(res)["error"]["code"] == kMethodNotFound);
    }
}

TEST_CASE("HTTP Host and Origin checks", "[mcp][http]") {
    SECTION("loopback hosts pass, others get 403") {
        Fixture f;
        for (const char* host : {"127.0.0.1", "localhost:8383", "[::1]:8383", "LOCALHOST"}) {
            auto req = f.post(Fixture::rpc("ping"));
            req.headers["Host"] = host;
            CHECK(f.endpoint.handle(req).status == 200);
        }
        auto req = f.post(Fixture::rpc("ping"));
        req.headers["Host"] = "evil.example:8383";
        CHECK(f.endpoint.handle(req).status == 403);
        req.headers.erase("Host");
        CHECK(f.endpoint.handle(req).status == 403);
    }
    SECTION("--allowed-hosts extends the list") {
        HttpEndpointOptions o;
        o.allowed_hosts = {"sap-vm.corp"};
        Fixture f(5000, 16, o);
        auto req = f.post(Fixture::rpc("ping"));
        req.headers["Host"] = "SAP-VM.corp:8443";
        CHECK(f.endpoint.handle(req).status == 200);
    }
    SECTION("Origin is rejected unless listed; CORS headers only for listed origins") {
        HttpEndpointOptions o;
        o.cors_origins = {"https://app.example"};
        Fixture f(5000, 16, o);
        auto req = f.post(Fixture::rpc("ping"));
        CHECK(f.endpoint.handle(req).header("Access-Control-Allow-Origin").empty());  // no Origin: no CORS
        req.headers["Origin"] = "https://evil.example";
        CHECK(f.endpoint.handle(req).status == 403);
        req.headers["Origin"] = "https://app.example";
        auto ok = f.endpoint.handle(req);
        CHECK(ok.status == 200);
        CHECK(ok.header("Access-Control-Allow-Origin") == "https://app.example");
        req.method = "OPTIONS";
        CHECK(f.endpoint.handle(req).status == 204);
    }
    SECTION("no CORS configured: any Origin is rejected") {
        Fixture f;
        auto req = f.post(Fixture::rpc("ping"));
        req.headers["Origin"] = "http://localhost:3000";
        CHECK(f.endpoint.handle(req).status == 403);
    }
}

// ---- authentication seam -----------------------------------------------------------------------------

TEST_CASE("HTTP authentication happens before the body is parsed", "[mcp][http][auth]") {
    Fixture f;
    SECTION("missing credentials: 401 + WWW-Authenticate + error body, even for garbage bodies") {
        auto req = f.post(json(), false);
        req.body = "{garbage";
        auto res = f.endpoint.handle(req);
        CHECK(res.status == 401);
        CHECK(res.header("WWW-Authenticate") == "Bearer realm=\"test\"");
        CHECK(body_of(res)["error_code"] == "AUTH_REQUIRED");
    }
    SECTION("invalid token is 403 and headers reach the authenticator") {
        auto req = f.post(Fixture::rpc("ping"), false);
        req.headers["Authorization"] = "Bearer bad";
        req.headers["X-Fairyfly-Proxy-Secret"] = "s3";
        req.headers["X-Forwarded-For"] = "10.0.0.9";
        req.headers["X-Forwarded-Proto"] = "https";
        auto res = f.endpoint.handle(req);
        CHECK(res.status == 403);
        CHECK(body_of(res)["error_code"] == "TOKEN_INVALID");
        CHECK(f.auth.last.proxy_secret == "s3");
        CHECK(f.auth.last.forwarded_for == "10.0.0.9");
        CHECK(f.auth.last.forwarded_proto == "https");
        CHECK(f.auth.last.peer_addr == "127.0.0.1");
    }
    SECTION("the principal reaches the provider with era and transport") {
        auto req = f.post(Fixture::rpc("tools/call", json{{"name", "gui_screen_read"}, {"_meta", Fixture::stateless_meta()}}));
        req.headers["X-Forwarded-For"] = "10.1.1.1";
        auto res = f.endpoint.handle(req);
        REQUIRE(res.status == 200);
        std::lock_guard<std::mutex> lock(f.provider.m);
        CHECK(f.provider.last_principal.name == "alice");
        CHECK(f.provider.last_principal.authenticated);
        CHECK(f.provider.last_principal.remote_addr == "10.1.1.1");
        CHECK(f.provider.last_era == ProtocolEra::Stateless);
        CHECK(f.provider.last_http);
        CHECK(f.provider.client_info["name"] == "tester");
    }
    SECTION("default DenyAllAuthenticator answers 401 with the token hint") {
        DenyAllAuthenticator deny;
        FakeProvider provider;
        CallExecutor exec(4, 1000);
        HttpEndpoint ep({}, exec, provider, &deny);
        HttpRequest req;
        req.method = "POST";
        req.path = "/mcp";
        req.headers["Host"] = "localhost";
        req.headers["Content-Type"] = "application/json";
        req.body = "{}";
        auto res = ep.handle(req);
        CHECK(res.status == 401);
        CHECK(body_of(res)["error_code"] == "AUTH_REQUIRED");
        CHECK(body_of(res)["message"].get<std::string>().find("fairyfly mcp token create") != std::string::npos);
        CHECK(ep.calls_denied() == 1);
    }
    SECTION("AllowAllAuthenticator yields the unauthenticated 'insecure' principal") {
        AllowAllAuthenticator allow;
        AuthRequest r;
        r.peer_addr = "127.0.0.1";
        auto out = allow.authenticate(r);
        CHECK(out.ok);
        CHECK(out.principal.name == "insecure");
        CHECK_FALSE(out.principal.authenticated);
        CHECK_FALSE(allow.posture_warnings().empty());
    }
    SECTION("factory seam selects the authenticator") {
        CHECK(dynamic_cast<AllowAllAuthenticator*>(make_http_authenticator(true).get()));
        CHECK(dynamic_cast<DenyAllAuthenticator*>(make_http_authenticator(false).get()));
    }
}

// ---- dual era ---------------------------------------------------------------------------------------

TEST_CASE("Stateless era: discover, resultType, headers, versions", "[mcp][http][era]") {
    Fixture f;
    SECTION("server/discover") {
        auto res = f.endpoint.handle(f.post(Fixture::rpc("server/discover", json{{"_meta", Fixture::stateless_meta()}})));
        REQUIRE(res.status == 200);
        const json result = body_of(res)["result"];
        CHECK(result["resultType"] == "complete");
        CHECK(result["supportedVersions"] == json::array({"2026-07-28", "2025-11-25", "2025-06-18"}));
        CHECK(result["serverInfo"]["name"] == "fairyfly");
        CHECK(result["serverInfo"]["version"] == "9.9.9");
        CHECK(result["instructions"] == "be careful");
        CHECK(result["capabilities"].contains("tools"));
        // discover works without _meta as well
        CHECK(f.endpoint.handle(f.post(Fixture::rpc("server/discover"))).status == 200);
    }
    SECTION("tools/list is deterministic, sorted, and carries ttl/cache metadata") {
        const json params{{"_meta", Fixture::stateless_meta()}};
        auto a = f.endpoint.handle(f.post(Fixture::rpc("tools/list", params)));
        auto b = f.endpoint.handle(f.post(Fixture::rpc("tools/list", params)));
        REQUIRE(a.status == 200);
        CHECK(a.body == b.body);
        const json result = body_of(a)["result"];
        CHECK(result["resultType"] == "complete");
        CHECK(result.contains("ttlMs"));
        CHECK(result.contains("cacheScope"));
        std::vector<std::string> names;
        for (const auto& t : result["tools"]) names.push_back(t["name"]);
        CHECK(std::is_sorted(names.begin(), names.end()));
        CHECK(names.size() == 5);
    }
    SECTION("tools/call result carries resultType complete") {
        auto res = f.endpoint.handle(f.post(Fixture::rpc(
            "tools/call", json{{"name", "gui_screen_read"}, {"arguments", json::object()}, {"_meta", Fixture::stateless_meta()}})));
        REQUIRE(res.status == 200);
        const json result = body_of(res)["result"];
        CHECK(result["resultType"] == "complete");
        CHECK(result["content"][0]["text"] == "ok:gui_screen_read");
        CHECK(result["structuredContent"]["tool"] == "gui_screen_read");
    }
    SECTION("Mcp-Method / Mcp-Name mismatch is 400 with -32020") {
        auto req = f.post(Fixture::rpc("tools/call", json{{"name", "gui_screen_read"}, {"_meta", Fixture::stateless_meta()}}));
        req.headers["Mcp-Method"] = "tools/list";
        auto res = f.endpoint.handle(req);
        CHECK(res.status == 400);
        CHECK(body_of(res)["error"]["code"] == kHeaderMismatch);
        req.headers["Mcp-Method"] = "tools/call";
        req.headers["Mcp-Name"] = "gui_other";
        res = f.endpoint.handle(req);
        CHECK(res.status == 400);
        CHECK(body_of(res)["error"]["code"] == -32020);
        req.headers["Mcp-Name"] = "gui_screen_read";
        CHECK(f.endpoint.handle(req).status == 200);
        {
            std::lock_guard<std::mutex> lock(f.provider.m);
            CHECK(f.provider.calls == 1);  // the mismatching requests never reached the provider
        }
    }
    SECTION("unsupported version is 400 with -32022 and the supported list") {
        json meta = Fixture::stateless_meta();
        meta["protocolVersion"] = "2099-01-01";
        auto res = f.endpoint.handle(f.post(Fixture::rpc("tools/list", json{{"_meta", meta}})));
        CHECK(res.status == 400);
        const json error = body_of(res)["error"];
        CHECK(error["code"] == kUnsupportedVersion);
        CHECK(error["data"]["supported"] == json::array({"2026-07-28", "2025-11-25", "2025-06-18"}));
        auto by_header = f.post(Fixture::rpc("tools/list"));
        by_header.headers["MCP-Protocol-Version"] = "1999-01-01";
        CHECK(body_of(f.endpoint.handle(by_header))["error"]["code"] == -32022);
    }
    SECTION("namespaced _meta keys are accepted") {
        json meta{{"io.modelcontextprotocol/protocolVersion", "2026-07-28"}, {"io.modelcontextprotocol/clientInfo", json{{"name", "ns"}}}};
        auto res = f.endpoint.handle(f.post(Fixture::rpc("tools/list", json{{"_meta", meta}})));
        CHECK(body_of(res)["result"]["resultType"] == "complete");
    }
}

TEST_CASE("HTTP tools/list follows the caller's principal", "[mcp][http][era]") {
    // A provider that filters by scope: tools whose name is gui_<scope>_... belong to that scope.
    class ScopedProvider : public FakeProvider {
    public:
        std::vector<ToolDef> list_tools_for(const Principal& principal) const override {
            std::vector<ToolDef> out;
            for (const auto& d : list_tools())
                if (principal.all_scopes || principal.scopes.count(d.name.substr(4, d.name.find('_', 4) - 4)) > 0) out.push_back(d);
            return out;
        }
    };
    ScopedProvider scoped;
    TokenAuth auth;  // "Bearer good" -> principal alice with scopes {"screen"}
    CallExecutor exec(16, 5000);
    HttpEndpointOptions opts;
    opts.server.name = "fairyfly";
    opts.server.version = "1";
    opts.poll_ms = 10;
    HttpEndpoint endpoint(opts, exec, scoped, &auth);
    std::thread loop([&] { exec.run(); });

    HttpRequest req;
    req.method = "POST";
    req.path = "/mcp";
    req.headers["Host"] = "127.0.0.1:8383";
    req.headers["Content-Type"] = "application/json";
    req.headers["Authorization"] = "Bearer good";
    req.peer_addr = "127.0.0.1";
    req.body = json{{"jsonrpc", "2.0"}, {"id", 1}, {"method", "tools/list"}}.dump();
    const auto res = endpoint.handle(req);
    exec.request_stop();
    loop.join();

    REQUIRE(res.status == 200);
    std::vector<std::string> names;
    const json parsed = json::parse(res.body);
    for (const auto& t : parsed["result"]["tools"]) names.push_back(t["name"]);
    // alice has the scope "screen": only gui_screen_read is visible, the rest of the fake catalog is hidden
    CHECK(names == std::vector<std::string>{"gui_screen_read"});
}

TEST_CASE("HTTP tools/list without a principal-aware provider stays complete and sorted", "[mcp][http][era]") {
    Fixture f;  // FakeProvider keeps the default list_tools_for() (= list_tools())
    const auto res = f.endpoint.handle(f.post(Fixture::rpc("tools/list")));
    REQUIRE(res.status == 200);
    std::vector<std::string> names;
    const json parsed = body_of(res);
    for (const auto& t : parsed["result"]["tools"]) names.push_back(t["name"]);
    CHECK(names.size() == 5);
    CHECK(std::is_sorted(names.begin(), names.end()));
}

TEST_CASE("initialize negotiates against all served versions; the era follows", "[mcp][http][era]") {
    Fixture f;
    // stateless version: negotiated as such and answered in the stateless era
    auto res = f.endpoint.handle(f.post(Fixture::rpc("initialize", json{{"protocolVersion", "2026-07-28"},
                                                                           {"clientInfo", json{{"name", "c"}}}})));
    REQUIRE(res.status == 200);
    json result = body_of(res)["result"];
    CHECK(result["protocolVersion"] == "2026-07-28");
    CHECK(result["resultType"] == "complete");
    CHECK(result["serverInfo"]["name"] == "fairyfly");
    CHECK(res.header("Mcp-Session-Id").empty());
    // the two legacy versions keep working and are answered in the legacy shape
    for (const char* version : {"2025-11-25", "2025-06-18"}) {
        res = f.endpoint.handle(f.post(Fixture::rpc("initialize", json{{"protocolVersion", version}})));
        result = body_of(res)["result"];
        CHECK(result["protocolVersion"] == version);
        CHECK_FALSE(result.contains("resultType"));
    }
}

TEST_CASE("Legacy era: initialize, no session id, no prior initialize needed", "[mcp][http][era]") {
    Fixture f;
    SECTION("initialize negotiates and mints no session") {
        for (const char* version : {"2025-11-25", "2025-06-18"}) {
            auto res = f.endpoint.handle(f.post(Fixture::rpc(
                "initialize", json{{"protocolVersion", version}, {"clientInfo", json{{"name", "c"}, {"version", "1"}}},
                                   {"capabilities", json::object()}})));
            REQUIRE(res.status == 200);
            CHECK(res.header("Mcp-Session-Id").empty());
            const json result = body_of(res)["result"];
            CHECK(result["protocolVersion"] == version);
            CHECK(result["serverInfo"]["name"] == "fairyfly");
            CHECK(result["capabilities"]["tools"]["listChanged"] == false);
            CHECK_FALSE(result.contains("resultType"));
        }
        // an unknown version falls back to the newest legacy version, still a legacy answer
        auto unknown = f.endpoint.handle(f.post(Fixture::rpc("initialize", json{{"protocolVersion", "2031-01-01"}})));
        CHECK(body_of(unknown)["result"]["protocolVersion"] == "2025-11-25");
        CHECK_FALSE(body_of(unknown)["result"].contains("resultType"));
        auto bad = f.endpoint.handle(f.post(Fixture::rpc("initialize", json::object())));
        CHECK(body_of(bad)["error"]["code"] == kInvalidParams);
    }
    SECTION("tools/list and tools/call work without initialize, results have no resultType") {
        auto list = f.endpoint.handle(f.post(Fixture::rpc("tools/list")));
        REQUIRE(list.status == 200);
        CHECK_FALSE(body_of(list)["result"].contains("resultType"));
        auto call = f.endpoint.handle(f.post(Fixture::rpc("tools/call", json{{"name", "gui_a_tool"}})));
        REQUIRE(call.status == 200);
        CHECK(body_of(call)["result"]["content"][0]["text"] == "ok:gui_a_tool");
        std::lock_guard<std::mutex> lock(f.provider.m);
        CHECK(f.provider.last_era == ProtocolEra::Legacy);
        CHECK(f.provider.client_info.is_object());  // reset per request (no clientInfo leaks between clients)
    }
    SECTION("MCP-Protocol-Version header selects the era") {
        auto req = f.post(Fixture::rpc("tools/call", json{{"name", "gui_a_tool"}}));
        req.headers["MCP-Protocol-Version"] = "2025-06-18";
        CHECK_FALSE(body_of(f.endpoint.handle(req))["result"].contains("resultType"));
    }
}

TEST_CASE("Tool error semantics are the same as on stdio", "[mcp][http]") {
    Fixture f;
    SECTION("unknown tool is a protocol error -32602") {
        auto res = f.endpoint.handle(f.post(Fixture::rpc("tools/call", json{{"name", "gui_nope"}})));
        CHECK(res.status == 200);
        CHECK(body_of(res)["error"]["code"] == kInvalidParams);
    }
    SECTION("hidden-but-known tool reaches the provider and returns isError") {
        auto res = f.endpoint.handle(f.post(Fixture::rpc("tools/call", json{{"name", "gui_element_fill"}})));
        REQUIRE(res.status == 200);
        CHECK(body_of(res)["result"]["isError"] == true);
        CHECK(body_of(res)["result"]["content"][0]["text"].get<std::string>().find("TOOL_UNAVAILABLE_READ_ONLY") != std::string::npos);
    }
    SECTION("bad params") {
        CHECK(body_of(f.endpoint.handle(f.post(Fixture::rpc("tools/call", json::object()))))["error"]["code"] == kInvalidParams);
        CHECK(body_of(f.endpoint.handle(f.post(Fixture::rpc("tools/call", json{{"name", "gui_a_tool"}, {"arguments", 5}}))))["error"]["code"] == kInvalidParams);
        CHECK(body_of(f.endpoint.handle(f.post(Fixture::rpc("tools/list", json{{"cursor", "x"}}))))["error"]["code"] == kInvalidParams);
    }
}

// ---- executor: timeout, busy, queue ---------------------------------------------------------------------

TEST_CASE("Executor: soft timeout, SERVER_BUSY and queue cap", "[mcp][http][executor]") {
    SECTION("a timed-out call is answered CALL_TIMEOUT and a concurrent tools/call gets SERVER_BUSY") {
        Fixture f(100);
        auto slow = std::async(std::launch::async, [&] {
            return f.endpoint.handle(f.post(Fixture::rpc("tools/call", json{{"name", "gui_slow"}, {"arguments", json{{"ms", 600}}}})));
        });
        std::this_thread::sleep_for(300ms);  // past the 100 ms deadline, call still running
        auto busy = f.endpoint.handle(f.post(Fixture::rpc("tools/call", json{{"name", "gui_a_tool"}}, 2)));
        CHECK(body_of(busy)["result"]["content"][0]["text"].get<std::string>().find("SERVER_BUSY") != std::string::npos);
        REQUIRE(slow.wait_for(5s) == std::future_status::ready);
        auto timed_out = slow.get();
        CHECK(body_of(timed_out)["result"]["isError"] == true);
        CHECK(body_of(timed_out)["result"]["content"][0]["text"].get<std::string>().find("CALL_TIMEOUT") != std::string::npos);
    }
    SECTION("queue cap is honoured (503 -32000)") {
        Fixture f(5000, 1);
        auto first = std::async(std::launch::async, [&] {
            return f.endpoint.handle(f.post(Fixture::rpc("tools/call", json{{"name", "gui_slow"}, {"arguments", json{{"ms", 400}}}})));
        });
        std::this_thread::sleep_for(100ms);  // first is running, queue is empty
        auto second = std::async(std::launch::async, [&] {
            return f.endpoint.handle(f.post(Fixture::rpc("tools/call", json{{"name", "gui_a_tool"}}, 2)));
        });
        std::this_thread::sleep_for(100ms);  // second sits in the queue (cap 1)
        auto third = f.endpoint.handle(f.post(Fixture::rpc("tools/call", json{{"name", "gui_a_tool"}}, 3)));
        CHECK(third.status == 503);
        CHECK(body_of(third)["error"]["code"] == kServerBusy);
        REQUIRE(first.wait_for(5s) == std::future_status::ready);
        REQUIRE(second.wait_for(5s) == std::future_status::ready);
        CHECK(second.get().status == 200);
    }
    SECTION("submit_future returns the delivered message") {
        CallExecutor exec(4, 1000);
        std::thread loop([&] { exec.run(); });
        ExecJob job;
        job.id = 1;
        job.run = [](CallState&) { return make_result(1, json{{"v", 1}}); };
        SubmitResult submitted;
        auto fut = exec.submit_future(std::move(job), &submitted);
        REQUIRE(submitted == SubmitResult::Queued);
        REQUIRE(fut.wait_for(5s) == std::future_status::ready);
        CHECK(fut.get()["result"]["v"] == 1);
        exec.request_stop();
        loop.join();
    }
}

// ---- SSE ------------------------------------------------------------------------------------------------

TEST_CASE("SSE negotiation honours q-values and progressToken", "[mcp][http][sse]") {
    // JSON and SSE equally acceptable, no progress token -> plain JSON
    CHECK_FALSE(accept_prefers_sse("application/json, text/event-stream", false));
    CHECK_FALSE(accept_prefers_sse("text/event-stream, application/json", false));
    CHECK_FALSE(accept_prefers_sse("Application/JSON;q=0.9, TEXT/event-stream;q=0.9", false));
    CHECK_FALSE(accept_prefers_sse("application/json", false));
    CHECK_FALSE(accept_prefers_sse("*/*", false));
    CHECK_FALSE(accept_prefers_sse("", false));
    // only SSE listed
    CHECK(accept_prefers_sse("text/event-stream", false));
    CHECK(accept_prefers_sse("text/event-stream;q=0.5", false));
    // higher q for SSE
    CHECK(accept_prefers_sse("application/json;q=0.5, text/event-stream", false));
    CHECK_FALSE(accept_prefers_sse("application/json, text/event-stream;q=0.5", false));
    // wildcard counts as JSON acceptable
    CHECK_FALSE(accept_prefers_sse("text/event-stream, */*", false));
    CHECK(accept_prefers_sse("text/event-stream, */*;q=0.1", false));
    // progress token -> SSE whenever it is acceptable at all
    CHECK(accept_prefers_sse("application/json, text/event-stream", true));
    CHECK(accept_prefers_sse("application/json;q=1, text/event-stream;q=0.1", true));
    CHECK_FALSE(accept_prefers_sse("application/json", true));
    CHECK_FALSE(accept_prefers_sse("application/json, text/event-stream;q=0", true));
    CHECK_FALSE(accept_prefers_sse("*/*", true));
}

TEST_CASE("SSE: both Accept types without progressToken answer plain JSON", "[mcp][http][sse]") {
    Fixture f;
    auto req = f.post(Fixture::rpc("tools/call", json{{"name", "gui_a_tool"}}));
    req.headers["Accept"] = "application/json, text/event-stream";
    auto res = f.endpoint.handle(req);
    REQUIRE(res.status == 200);
    CHECK(res.header("Content-Type").find("application/json") != std::string::npos);
    CHECK_FALSE(res.stream);

    auto only_sse = f.post(Fixture::rpc("tools/call", json{{"name", "gui_a_tool"}}));
    only_sse.headers["Accept"] = "text/event-stream";
    auto res2 = f.endpoint.handle(only_sse);
    CHECK(res2.header("Content-Type") == "text/event-stream");
}

TEST_CASE("SSE: progress, final message, keep-alive", "[mcp][http][sse]") {
    HttpEndpointOptions o;
    o.keepalive_ms = 40;
    Fixture f(5000, 16, o);
    auto req = f.post(Fixture::rpc("tools/call", json{{"name", "gui_a_tool"}, {"_meta", json{{"progressToken", "tok"}}}}));
    req.headers["Accept"] = "application/json, text/event-stream";
    auto res = f.endpoint.handle(req);
    REQUIRE(res.status == 200);
    CHECK(res.header("Content-Type") == "text/event-stream");
    CHECK(res.header("X-Accel-Buffering") == "no");
    CHECK(res.header("Cache-Control") == "no-cache");
    REQUIRE(res.stream);
    VectorSink sink;
    res.stream(sink);
    const std::string all = sink.all();
    CHECK(all.find("notifications/progress") != std::string::npos);
    CHECK(all.find("\"progressToken\":\"tok\"") != std::string::npos);
    CHECK(all.find("started") != std::string::npos);
    CHECK(all.find("half") != std::string::npos);
    CHECK(all.find("finished") != std::string::npos);
    const auto final_pos = all.rfind("event: message\ndata: {\"id\":1");
    REQUIRE(final_pos != std::string::npos);
    CHECK(all.find("ok:gui_a_tool") > all.find("started"));  // progress precedes the final message

    SECTION("--no-sse and missing Accept fall back to plain JSON") {
        HttpEndpointOptions off;
        off.sse = false;
        Fixture g(5000, 16, off);
        auto r = g.post(Fixture::rpc("tools/call", json{{"name", "gui_a_tool"}}));
        r.headers["Accept"] = "text/event-stream";
        auto plain = g.endpoint.handle(r);
        CHECK_FALSE(plain.stream);
        CHECK(plain.header("Content-Type") == "application/json");
        auto no_accept = f.post(Fixture::rpc("tools/call", json{{"name", "gui_a_tool"}}));
        CHECK_FALSE(f.endpoint.handle(no_accept).stream);
    }
}

TEST_CASE("SSE: keep-alive comments while a call runs", "[mcp][http][sse]") {
    HttpEndpointOptions o;
    o.keepalive_ms = 40;
    Fixture f(5000, 16, o);
    auto req = f.post(Fixture::rpc("tools/call", json{{"name", "gui_slow"}, {"arguments", json{{"ms", 300}}}}));
    req.headers["Accept"] = "text/event-stream";
    auto res = f.endpoint.handle(req);
    REQUIRE(res.stream);
    VectorSink sink;
    res.stream(sink);
    const std::string all = sink.all();
    CHECK(all.find(": keep-alive\n\n") != std::string::npos);
    CHECK(all.find("event: message") != std::string::npos);
}

TEST_CASE("SSE: client disconnect sets the call's cancelled flag", "[mcp][http][sse]") {
    HttpEndpointOptions o;
    o.keepalive_ms = 0;
    Fixture f(5000, 16, o);
    auto req = f.post(Fixture::rpc("tools/call", json{{"name", "gui_wait_cancel"}}));
    req.headers["Accept"] = "text/event-stream";
    auto res = f.endpoint.handle(req);
    REQUIRE(res.stream);
    VectorSink sink;
    std::thread dropper([&] {
        std::this_thread::sleep_for(150ms);
        sink.gone = true;  // the client went away
    });
    res.stream(sink);
    dropper.join();
    // The provider observes the flag between its steps and returns (no hang until its 3 s bound).
    const auto until = std::chrono::steady_clock::now() + 2s;
    while (!f.provider.saw_cancel && std::chrono::steady_clock::now() < until) std::this_thread::sleep_for(10ms);
    CHECK(f.provider.saw_cancel.load());
}

// ---- IServerControl + loopback smoke ------------------------------------------------------------------------

TEST_CASE("Loopback: cpp-httplib client against the real adapter", "[mcp][http][loopback]") {
    FakeProvider provider;
    HttpServerConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.endpoint.keepalive_ms = 5000;
    config.endpoint.server.name = "fairyfly";
    config.endpoint.server.version = "1";
    config.read_only = true;
    std::atomic<int> applied{-1};
    McpHttpServer server(config, provider, std::make_unique<TokenAuth>(),
                         [&applied](bool ro) { applied = ro ? 1 : 0; });
    std::string error;
    REQUIRE(server.bind(&error));
    const int port = server.port();
    REQUIRE(port > 0);

    struct Outcome {
        int ping = 0, unauth = 0, get = 0, wrong_host = 0, ctype = 0, sse_status = 0;
        std::string list_body, sse_body, sse_ctype;
        ServerStatus status_before, status_after;
    } out;

    std::atomic<bool> finished{false};
    // Hard bound: a hang can never outlive the test (30 s), and the guard is always joined.
    std::thread guard([&] {
        for (int i = 0; i < 300 && !finished; ++i) std::this_thread::sleep_for(100ms);
        if (!finished) server.request_stop();
    });
    std::thread client([&] {
        httplib::Client cli("127.0.0.1", port);
        cli.set_connection_timeout(3, 0);
        cli.set_read_timeout(10, 0);
        const httplib::Headers auth{{"Authorization", "Bearer good"}};
        // wait for the listener (run() starts it right after we begin)
        for (int i = 0; i < 100; ++i) {
            if (server.status().running) break;
            std::this_thread::sleep_for(20ms);
        }
        out.status_before = server.status();
        if (auto r = cli.Post("/mcp", auth, R"({"jsonrpc":"2.0","id":1,"method":"ping"})", "application/json")) out.ping = r->status;
        if (auto r = cli.Post("/mcp", R"({"jsonrpc":"2.0","id":1,"method":"ping"})", "application/json")) out.unauth = r->status;
        if (auto r = cli.Get("/mcp", auth)) out.get = r->status;
        if (auto r = cli.Post("/mcp", auth, "x", "text/plain")) out.ctype = r->status;
        {
            httplib::Headers h = auth;
            h.emplace("Host", "evil.example");
            httplib::Client c2("127.0.0.1", port);
            c2.set_read_timeout(5, 0);
            if (auto r = c2.Post("/mcp", h, R"({"jsonrpc":"2.0","id":1,"method":"ping"})", "application/json")) out.wrong_host = r->status;
        }
        if (auto r = cli.Post("/mcp", auth,
                              R"({"jsonrpc":"2.0","id":2,"method":"tools/list","params":{"_meta":{"protocolVersion":"2026-07-28"}}})",
                              "application/json"))
            out.list_body = r->body;
        {
            httplib::Headers h = auth;
            h.emplace("Accept", "text/event-stream");
            if (auto r = cli.Post("/mcp", h,
                                  R"({"jsonrpc":"2.0","id":3,"method":"tools/call","params":{"name":"gui_a_tool","_meta":{"progressToken":"p1"}}})",
                                  "application/json")) {
                out.sse_status = r->status;
                out.sse_body = r->body;
                out.sse_ctype = r->get_header_value("Content-Type");
            }
        }
        server.set_read_only(false);  // write mode is allowed (no env cap): applied on the main thread
        std::this_thread::sleep_for(200ms);
        out.status_after = server.status();
        server.request_stop();
    });

    const int exit_code = server.run();  // this (test) thread is the "main" thread
    finished = true;
    client.join();
    guard.join();

    CHECK(exit_code == 0);
    CHECK(out.ping == 200);
    CHECK(out.unauth == 401);
    CHECK(out.get == 405);
    CHECK(out.ctype == 415);
    CHECK(out.wrong_host == 403);
    REQUIRE_FALSE(out.list_body.empty());
    CHECK(json::parse(out.list_body)["result"]["resultType"] == "complete");
    CHECK(out.sse_status == 200);
    CHECK(out.sse_ctype.find("text/event-stream") != std::string::npos);
    CHECK(out.sse_body.find("event: message") != std::string::npos);
    CHECK(out.sse_body.find("notifications/progress") != std::string::npos);
    CHECK(out.status_before.read_only);
    CHECK_FALSE(out.status_after.read_only);
    CHECK(applied.load() == 0);
    CHECK(out.status_after.calls_total >= 1);
    CHECK(out.status_after.endpoint == "http://127.0.0.1:" + std::to_string(port) + "/mcp");
    CHECK_FALSE(server.restart_requested());
}

TEST_CASE("IServerControl: read-only cap, restart flag, posture banner", "[mcp][http][control]") {
    FakeProvider provider;
    HttpServerConfig config;
    config.port = 0;
    config.read_only = true;
    config.read_only_cap = true;
    config.host = "0.0.0.0";
    config.insecure_no_auth = true;
    std::atomic<int> applied{-1};
    McpHttpServer server(config, provider, std::make_unique<AllowAllAuthenticator>(), [&](bool ro) { applied = ro; });
    server.set_read_only(false);  // refused by the cap
    CHECK(server.status().read_only);
    CHECK(applied.load() == -1);

    std::string banner;
    for (const auto& line : server.posture_lines()) banner += line + "\n";
    CHECK(banner.find("read-only guard") != std::string::npos);
    CHECK(banner.find("non-loopback") != std::string::npos);
    CHECK(banner.find("--insecure-no-auth") != std::string::npos);
    CHECK(banner.find("stateless 2026-07-28") != std::string::npos);
    CHECK(banner.find("sse:") != std::string::npos);
    CHECK(banner.find("FAIRYFLY_READ_ONLY") != std::string::npos);

    REQUIRE(server.bind());
    std::thread stopper([&] {
        std::this_thread::sleep_for(100ms);
        server.request_restart();
    });
    CHECK(server.run() == 0);
    stopper.join();
    CHECK(server.restart_requested());
    CHECK_FALSE(server.status().running);
}

TEST_CASE("posture banner prints the insecure-auth warning once", "[mcp][http][banner]") {
    const auto count_auth_warnings = [](const std::vector<std::string>& lines) {
        int n = 0;
        for (const auto& l : lines) {
            if (l.find("WARNING:") == std::string::npos) continue;
            std::string lower = l;
            std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (lower.find("authentication") != std::string::npos && lower.find("disabled") != std::string::npos) ++n;
        }
        return n;
    };
    FakeProvider provider;
    HttpServerConfig config;
    config.port = 0;
    config.insecure_no_auth = true;
    McpHttpServer with_allow_all(config, provider, std::make_unique<AllowAllAuthenticator>(), [](bool) {});
    CHECK(count_auth_warnings(with_allow_all.posture_lines()) == 1);

    // an authenticator without its own warning still gets the server's line
    struct Silent final : IAuthenticator {
        AuthOutcome authenticate(const AuthRequest&) override { return {}; }
    };
    McpHttpServer with_silent(config, provider, std::make_unique<Silent>(), [](bool) {});
    CHECK(count_auth_warnings(with_silent.posture_lines()) == 1);
}

#ifdef _WIN32
namespace {
/// Opens a raw TCP connection to 127.0.0.1:port and sends `data` (no further bytes, no close).
SOCKET raw_connect_send(int port, const std::string& data) {
    SOCKET s = ::socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s == INVALID_SOCKET) return s;
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<u_short>(port));
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (::connect(s, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        ::closesocket(s);
        return INVALID_SOCKET;
    }
    ::send(s, data.data(), static_cast<int>(data.size()), 0);
    return s;
}
} // namespace

TEST_CASE("Loopback: unauthenticated slow bodies cannot exhaust the worker pool", "[mcp][http][loopback][loris]") {
    FakeProvider provider;
    HttpServerConfig config;
    config.host = "127.0.0.1";
    config.port = 0;
    config.worker_threads = 4;  // the adapter keeps at least half of them out of reach of body-draining clients
    config.endpoint.server.name = "fairyfly";
    config.endpoint.server.version = "1";
    McpHttpServer server(config, provider, std::make_unique<TokenAuth>());
    std::string error;
    REQUIRE(server.bind(&error));
    const int port = server.port();

    struct Outcome {
        int loris_open = 0;
        int legit_status = 0;
        long long legit_ms = -1;
        int bad_auth_ok = 0, bad_auth_total = 0;
        int drained_401 = 0;
        int good_after = 0;
    } out;

    std::atomic<bool> finished{false};
    std::thread guard([&] {
        for (int i = 0; i < 300 && !finished; ++i) std::this_thread::sleep_for(100ms);
        if (!finished) server.request_stop();
    });
    std::thread client([&] {
        for (int i = 0; i < 100 && !server.status().running; ++i) std::this_thread::sleep_for(20ms);

        // slow-loris: more sockets than workers, each announces a body but never sends it, none is authenticated
        const std::string headers = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1\r\nContent-Type: application/json\r\n"
                                    "Content-Length: 1000\r\n\r\n";
        std::vector<SOCKET> loris;
        for (int i = 0; i < 12; ++i) {
            SOCKET s = raw_connect_send(port, headers);
            if (s != INVALID_SOCKET) loris.push_back(s);
        }
        out.loris_open = static_cast<int>(loris.size());
        std::this_thread::sleep_for(300ms);

        // a legitimate authenticated request is still answered promptly
        {
            httplib::Client cli("127.0.0.1", port);
            cli.set_connection_timeout(3, 0);
            cli.set_read_timeout(8, 0);
            const auto start = std::chrono::steady_clock::now();
            if (auto r = cli.Post("/mcp", httplib::Headers{{"Authorization", "Bearer good"}},
                                  R"({"jsonrpc":"2.0","id":1,"method":"ping"})", "application/json"))
                out.legit_status = r->status;
            out.legit_ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        }
        // loris with a valid Content-Length but a wrong token is refused as well
        for (SOCKET s : loris) ::closesocket(s);

        // rejected requests that DO send their body must still receive the 401 (no connection reset)
        const std::string big(2048, 'x');
        for (int i = 0; i < 20; ++i) {
            httplib::Client cli("127.0.0.1", port);
            cli.set_connection_timeout(3, 0);
            cli.set_read_timeout(5, 0);
            ++out.bad_auth_total;
            if (auto r = cli.Post("/mcp", httplib::Headers{{"Authorization", "Bearer wrong"}}, big, "application/json")) {
                if (r->status == 403) ++out.bad_auth_ok;  // the test authenticator answers 403 TOKEN_INVALID
                if (r->body.find("TOKEN_INVALID") != std::string::npos) ++out.drained_401;
            }
        }
        {
            httplib::Client cli("127.0.0.1", port);
            if (auto r = cli.Post("/mcp", httplib::Headers{{"Authorization", "Bearer good"}},
                                  R"({"jsonrpc":"2.0","id":2,"method":"ping"})", "application/json"))
                out.good_after = r->status;
        }
        server.request_stop();
    });

    const int exit_code = server.run();
    finished = true;
    client.join();
    guard.join();

    CHECK(exit_code == 0);
    CHECK(out.loris_open == 12);
    CHECK(out.legit_status == 200);
    CHECK(out.legit_ms >= 0);
    CHECK(out.legit_ms < 3000);
    CHECK(out.bad_auth_ok == out.bad_auth_total);
    CHECK(out.drained_401 == out.bad_auth_total);
    CHECK(out.good_after == 200);
}
#endif

TEST_CASE("Session core: initialize/notifications state machine is unchanged", "[mcp][http][session]") {
    FakeProvider provider;
    ServerOptions options;
    options.version = "1";
    ProtocolSession session(provider, options);
    auto no_cancel = [] { return false; };
    Pending list{1, "tools/list", json::object()};
    CHECK(session.process(list, no_cancel)["error"]["code"] == kServerNotInitialized);
    Pending init{2, "initialize", json{{"protocolVersion", "2025-06-18"}, {"clientInfo", json{{"name", "x"}}}}};
    CHECK(session.process(init, no_cancel)["result"]["protocolVersion"] == "2025-06-18");
    CHECK(session.process(init, no_cancel)["error"]["code"] == kInvalidRequest);
    CHECK(session.process(Pending{nullptr, "notifications/initialized", json()}, no_cancel).is_null());
    CHECK(session.ready());
    CHECK(session.process(list, no_cancel)["result"]["tools"].size() == 5);
    CHECK(session.process(Pending{3, "server/discover", json()}, no_cancel)["error"]["code"] == kMethodNotFound);
}

// ---- serialised tool calls ----------------------------------------------------------------------
// The dispatcher's per-principal state machine (T-code blocked check -> invoke -> update) relies on this:
// tools/call reaches ToolProvider::call_tool only from the executor thread, one call at a time.

namespace {
class OverlapProvider : public ToolProvider {
public:
    std::vector<ToolDef> list_tools() const override { return {}; }
    bool has_tool(const std::string&) const override { return true; }
    ToolResult call_tool(const std::string&, const json&, const CallContext&) override {
        const int now = ++inside;
        int seen = max_inside.load();
        while (now > seen && !max_inside.compare_exchange_weak(seen, now)) {}
        {
            std::lock_guard<std::mutex> lock(m);
            threads.insert(std::this_thread::get_id());
        }
        std::this_thread::sleep_for(2ms);
        --inside;
        ++total;
        ToolResult r;
        r.content = json::array({json{{"type", "text"}, {"text", "ok"}}});
        return r;
    }
    std::atomic<int> inside{0}, max_inside{0}, total{0};
    std::mutex m;
    std::set<std::thread::id> threads;
};
} // namespace

TEST_CASE("CallExecutor: concurrent submitters never overlap inside call_tool", "[mcp][http][executor][serial]") {
    OverlapProvider provider;
    CallExecutor exec(64, 5000);
    std::thread loop([&] { exec.run(); });
    const std::thread::id loop_id = loop.get_id();

    constexpr int kPerThread = 15;
    auto submitter = [&](int base, std::vector<std::future<json>>& out) {
        for (int i = 0; i < kPerThread; ++i) {
            ExecJob job;
            job.id = base + i;
            job.timed = true;
            const json id = job.id;
            job.run = [&provider, id](CallState&) {
                Pending p{id, "tools/call", json{{"name", "gui_x"}, {"arguments", json::object()}}};
                return call_tool_message(provider, p, CallContext{});
            };
            SubmitResult res;
            auto fut = exec.submit_future(std::move(job), &res);
            REQUIRE(res == SubmitResult::Queued);
            out.push_back(std::move(fut));
        }
    };
    std::vector<std::future<json>> a, b;
    std::thread t1([&] { submitter(0, a); });
    std::thread t2([&] { submitter(100, b); });
    t1.join();
    t2.join();
    for (auto* set : {&a, &b})
        for (auto& f : *set) REQUIRE(f.wait_for(10s) == std::future_status::ready);

    CHECK(provider.total == 2 * kPerThread);
    CHECK(provider.max_inside == 1);
    CHECK(provider.threads.size() == 1);
    CHECK(provider.threads.count(loop_id) == 1);  // ran on the executor thread, not on a submitter

    exec.request_stop();
    loop.join();
}
