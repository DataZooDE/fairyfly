// fairyfly's proxy sources end to end against a real squid (Linux; CI installs squid, a SKIP fails the run): the
// decision of include/link/proxy_config.h installed into libffl as on Windows, with HTTPS requests and the link's
// WebSocket through real CONNECT tunnels. What is Windows-only (registry, WinHTTP's PAC/WPAD, SSPI) is replaced by
// its inputs here and tested on Windows CI (tests/unit/test_proxy_sources_win32.cpp, test_system_proxy.cpp).
#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <cstdlib>
#include <memory>

#include "http_client.hpp"
#include "include/link/link_control.h"
#include "include/link/proxy_config.h"
#include "squid.hpp"
#include "ws_client.hpp"

using namespace fairyfly::link::proxycfg;
using namespace ffy::test;
using Catch::Matchers::ContainsSubstring;

namespace {

#define NEED(sq) \
    if (!(sq).ok()) SKIP((sq).why())

// The decision installed into libffl for one test, its inputs changeable while it runs (like the registry).
struct Installed {
    std::shared_ptr<Inputs> in = std::make_shared<Inputs>();
    std::shared_ptr<std::string> fingerprint = std::make_shared<std::string>("0");
    std::shared_ptr<RouteResolver> resolver;
    Installed(AuthSources auth = {}, AutoProxy scripts = {}, EnvOverrides env = {}) {
        in->env = env;
        RouteResolver::Sources s;
        s.inputs = [in = in] { return *in; };
        s.autoproxy = std::move(scripts);
        s.fingerprint = [fp = fingerprint] { return *fp; };
        resolver = std::make_shared<RouteResolver>(s);
        ffy::proxy::configure(make_libffl_config(resolver, env, std::move(auth)));
    }
    ~Installed() { ffy::proxy::configure({}); }
};

Setting manual(const Squid& sq) {
    Setting s;
    s.mode = Mode::manual;
    s.server = "127.0.0.1:" + std::to_string(sq.port());
    return s;
}

std::string get_error(const std::string& url) {
    try {
        ffy::HttpRequest rq;
        rq.url = url;
        rq.timeout_ms = 5000;
        ffy::http_do(rq);
    } catch (const std::exception& e) {
        return e.what();
    }
    return "";
}

int get_status(const std::string& url) {
    ffy::HttpRequest rq;
    rq.url = url;
    rq.timeout_ms = 5000;
    return ffy::http_do(rq).status;
}

ProbeTargets targets(const TlsTarget& t, const std::string& host = kProxiedHost) {
    return probe_targets(t.wss(host), "https://unused.example");
}

}  // namespace

TEST_CASE("proxy squid: your Manual proxy carries HTTPS and the WebSocket, and beats HTTPS_PROXY") {
    Squid sq;
    NEED(sq);
    TlsTarget t;
    EnvGuard env{{"SSL_CERT_FILE", t.ca_file().c_str()}};
    EnvOverrides e;
    e.https_proxy = "http://127.0.0.1:1";  // would fail: never used while your setting decides
    Installed inst({}, {}, e);
    inst.in->user = manual(sq);

    const auto r = probe(targets(t), 5000, [&](const std::string& u) { return inst.resolver->decision(u); }, false);
    INFO(r.summary);
    CHECK(r.ok);
    CHECK(r.https == "ok (HTTP 200)");
    CHECK(r.websocket == "ok (upgraded)");
    CHECK_THAT(r.route, ContainsSubstring("via 127.0.0.1:" + std::to_string(sq.port()) + " from your fairyfly setting, Windows login: yes"));
    CHECK(sq.tunnelled(t.hostport()));
    CHECK(t.hits == 2);

    // without your setting HTTPS_PROXY decides (and is unreachable here)
    inst.in->user.reset();
    *inst.fingerprint = "1";
    CHECK_THAT(get_error(t.https()), ContainsSubstring("cannot reach proxy 127.0.0.1:1"));
}

TEST_CASE("proxy squid: the administrator's policy beats your setting") {
    Squid admin, mine;
    NEED(admin);
    NEED(mine);
    TlsTarget t;
    EnvGuard env{{"SSL_CERT_FILE", t.ca_file().c_str()}};
    Installed inst;
    inst.in->user = manual(mine);
    inst.in->policy.present = true;
    inst.in->policy.setting = manual(admin);
    CHECK(get_status(t.https()) == 200);
    CHECK(admin.tunnelled(t.hostport()));
    CHECK_FALSE(mine.tunnelled(t.hostport()));
}

TEST_CASE("proxy squid: Direct goes direct although HTTPS_PROXY and a browser policy name a proxy") {
    Squid sq;
    NEED(sq);
    TlsTarget t;
    REQUIRE_FALSE(t.ip().empty());
    EnvGuard env{{"SSL_CERT_FILE", t.ca_file().c_str()}};
    EnvOverrides e;
    e.https_proxy = sq.url();
    Installed inst({}, {}, e);
    BrowserPolicy edge;
    edge.browser = "Edge";
    edge.where = "HKLM\\x";
    edge.kind = BrowserPolicy::Kind::fixed;
    edge.server = "127.0.0.1:" + std::to_string(sq.port());
    inst.in->browsers = {edge};
    inst.in->user = Setting{Mode::direct, "", "", ""};
    CHECK(get_status(t.https(t.ip())) == 200);
    auto ws = ffy::WsClient::connect(t.wss(t.ip()), "ffl.v1", 5000);
    ws->close();
    CHECK(sq.access_log().find("CONNECT") == std::string::npos);

    // back to Auto: HTTPS_PROXY (with NO_PROXY for other hosts) decides, then the browser policy
    inst.in->user.reset();
    *inst.fingerprint = "auto";
    CHECK(get_status(t.https()) == 200);
    CHECK(sq.tunnelled(t.hostport()));
    CHECK(inst.resolver->decision(t.https()).tier == Tier::environment);
    inst.in->env = {};
    CHECK(inst.resolver->decision(t.https()).source == "Edge policy (HKLM)");
}

TEST_CASE("proxy squid: HTTPS_PROXY's user:password go to that proxy, and only to it") {
    Squid basic(Squid::Basic);
    NEED(basic);
    TlsTarget t;
    EnvGuard env{{"SSL_CERT_FILE", t.ca_file().c_str()}};
    EnvOverrides e;
    e.https_proxy = std::string("http://") + Squid::kUser + ":" + Squid::kPassword + "@127.0.0.1:" + std::to_string(basic.port());
    {
        Installed inst({}, {}, e);
        CHECK(get_status(t.https()) == 200);
        CHECK(basic.tunnelled(t.hostport(), Squid::kUser));
    }
    // the same credentials are not offered to another proxy (your setting names a second Basic squid)
    Squid other(Squid::Basic);
    NEED(other);
    Installed inst({}, {}, e);
    inst.in->user = manual(other);
    const std::string err = get_error(t.https());
    CHECK_THAT(err, ContainsSubstring("proxy requires authentication (offers Basic)"));
    CHECK(err.find(Squid::kPassword) == std::string::npos);
}

TEST_CASE("proxy squid: a browser-policy proxy may get the login and a stored credential; one found by WPAD does not") {
    Squid ntlm(Squid::Ntlm);
    NEED(ntlm);
    TlsTarget t;
    EnvGuard env{{"SSL_CERT_FILE", t.ca_file().c_str()}};
    AuthSources auth;
    auth.sspi = [](const ffy::proxy::Endpoint&) -> std::unique_ptr<ffy::proxy::Authenticator> { return std::make_unique<TestNtlm>(); };
    const std::string proxy = "127.0.0.1:" + std::to_string(ntlm.port());
    // "WPAD" names the same squid: the stand-in for WinHTTP's detection
    Installed inst(auth, [proxy](const std::string&, fairyfly::link::sysproxy::AutoSource, const std::string&, bool) {
        fairyfly::link::sysproxy::AutoProxyResult r;
        r.ok = true;
        r.proxy = proxy;
        return r;
    });
    BrowserPolicy chrome;
    chrome.browser = "Chrome";
    chrome.where = "HKLM\\x";
    chrome.kind = BrowserPolicy::Kind::fixed;
    chrome.server = proxy;
    inst.in->browsers = {chrome};
    CHECK(get_status(t.https()) == 200);  // NTLM handshake on the tunnel's connection
    CHECK(ntlm.tunnelled(t.hostport()));

    inst.in->browsers[0].kind = BrowserPolicy::Kind::auto_detect;
    *inst.fingerprint = "wpad";
    CHECK_THAT(get_error(t.https()), ContainsSubstring("discovered automatically (WPAD): not sending credentials"));
    inst.in->discovered_login_policy = true;  // the administrator's AllowLoginToDiscoveredProxies is read by the authenticator,
    CHECK_THAT(get_error(t.https()), ContainsSubstring("discovered automatically"));  // not from the inputs: still refused

    Squid basic(Squid::Basic);
    NEED(basic);
    AuthSources stored;
    int reads = 0;
    stored.stored_login = [&](const std::string&) {
        ++reads;
        return std::optional<fairyfly::link::sysproxy::StoredLogin>({Squid::kUser, Squid::kPassword});
    };
    Installed b(stored);
    b.in->browsers = {chrome};
    b.in->browsers[0].server = "127.0.0.1:" + std::to_string(basic.port());
    CHECK(get_status(t.https()) == 200);
    CHECK(basic.tunnelled(t.hostport(), Squid::kUser));
    CHECK(reads == 1);
}

TEST_CASE("proxy squid: Test connection with a setting not saved yet, on its thread only") {
    Squid sq;
    NEED(sq);
    TlsTarget t;
    EnvGuard env{{"SSL_CERT_FILE", t.ca_file().c_str()}};
    Installed inst;
    Setting broken;
    broken.mode = Mode::manual;
    broken.server = "127.0.0.1:1";
    inst.in->user = broken;  // the saved one cannot work

    auto failed = probe(targets(t), 3000, [&](const std::string& u) { return inst.resolver->decision(u); }, false);
    CHECK_FALSE(failed.ok);
    CHECK_THAT(failed.summary, ContainsSubstring("cannot reach proxy 127.0.0.1:1"));

    const Setting candidate = manual(sq);
    ProbeResult ok;
    {
        ScopedCandidate c(candidate);
        ok = probe(targets(t), 5000, [&](const std::string& u) { return inst.resolver->decision(u, candidate); }, false);
    }
    INFO(ok.summary);
    CHECK(ok.ok);
    CHECK(sq.tunnelled(t.hostport()));
    CHECK_THAT(get_error(t.https()), ContainsSubstring("cannot reach proxy 127.0.0.1:1"));  // still the saved one
}

TEST_CASE("proxy squid: a changed setting reconnects the running link along the new route (supervisor)") {
    Squid a, b;
    NEED(a);
    NEED(b);
    TlsTarget t;
    EnvGuard env{{"SSL_CERT_FILE", t.ca_file().c_str()}};
    Installed inst;
    inst.in->user = manual(a);
    REQUIRE_FALSE(inst.resolver->setting_changed());

    char dir[] = "/tmp/ffy-sup-XXXXXX";
    REQUIRE(::mkdtemp(dir));
    const std::string state = std::string(dir) + "/state.json";
    std::atomic<bool> stop{false};
    std::atomic<int> connects{0};
    fairyfly::link::SupervisorOptions o;
    o.poll = std::chrono::milliseconds(50);
    o.reconnect_when = [&] { return inst.resolver->setting_changed(); };
    std::thread sup([&] {
        fairyfly::link::supervise(
            state,
            [&](const std::atomic<bool>& net_stop) {
                auto ws = ffy::WsClient::connect(t.wss(), "ffl.v1", 5000);  // the link's WebSocket, as Agent::run opens it
                ++connects;
                while (!net_stop) std::this_thread::sleep_for(std::chrono::milliseconds(20));
                ws->close();
            },
            stop, o);
    });
    CHECK(wait_for([&] { return connects >= 1; }, 8000));
    inst.in->user = manual(b);
    *inst.fingerprint = "b";  // `fairyfly proxy set` in another process
    CHECK(wait_for([&] { return connects >= 2; }, 8000));
    // squid logs a tunnel when it closes: the first one (through a) closed for the reconnect
    CHECK(wait_for([&] { return a.tunnelled(t.hostport()); }, 5000));
    CHECK_FALSE(b.tunnelled(t.hostport()));
    stop = true;
    sup.join();
    CHECK(wait_for([&] { return b.tunnelled(t.hostport()); }, 5000));
    CHECK(connects == 2);  // one reconnect, not a loop
    std::filesystem::remove_all(dir);
}
