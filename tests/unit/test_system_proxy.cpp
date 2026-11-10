// The Windows proxy settings for the cloud link (include/link/system_proxy.h): how WinHTTP's answers become libffl
// routes, the bypass lists, the order of the settings, the SSPI challenge choice; on Windows also the real SSPI
// handshake (NTLM and Negotiate against AcceptSecurityContext with this user's login) and the installed resolver.
// The CONNECT tunnel itself is tested in the gateway's libffl against a real squid (tests/test_proxy.cpp there).
#include <catch2/catch_test_macros.hpp>

#include <cstdlib>
#include <string>
#include <vector>

#include "include/link/system_proxy.h"
#include "util.hpp"  // libffl

#ifdef _WIN32
#include <windows.h>
#define SECURITY_WIN32
#include <security.h>
#endif

using namespace fairyfly::link::sysproxy;
using ffy::proxy::Route;
using ffy::proxy::Routes;

namespace {
Route via(const std::string& h, int p) { return Route::via({h, p}); }
const Route kDirect{};
AutoProxyResult pac(std::string proxy, std::string bypass = {}) {
    AutoProxyResult r;
    r.ok = true;
    r.proxy = std::move(proxy);
    r.bypass = std::move(bypass);
    return r;
}
}  // namespace

TEST_CASE("system proxy: WinHTTP proxy lists become routes for https:// in their order") {
    CHECK(parse_proxy_list("proxy.corp.example:8080") == Routes{via("proxy.corp.example", 8080)});
    CHECK(parse_proxy_list("proxy") == Routes{via("proxy", 80)});  // WinHTTP's default port
    CHECK(parse_proxy_list("http://proxy:3128/") == Routes{via("proxy", 3128)});
    // per-scheme lists: the https= entry; http= alone does not apply to https
    CHECK(parse_proxy_list("http=a:80;https=b:8443;ftp=c:21") == Routes{via("b", 8443)});
    CHECK(parse_proxy_list("http=a:80").empty());
    CHECK(parse_proxy_list("socks=s:1080").empty());
    // a PAC result: several proxies, DIRECT where the script put it
    CHECK(parse_proxy_list("p1:8080; p2:8080;DIRECT") == Routes{via("p1", 8080), via("p2", 8080), kDirect});
    CHECK(parse_proxy_list("PROXY p1:8080; DIRECT") == Routes{via("p1", 8080), kDirect});
    CHECK(parse_proxy_list("DIRECT").empty());
    CHECK(parse_proxy_list("").empty());
    CHECK(parse_proxy_list("[fd00::5]:3128") == Routes{via("[fd00::5]", 3128)});
    // not usable: TLS to the proxy, bad ports, garbage
    CHECK(parse_proxy_list("https://tlsproxy:443").empty());
    CHECK(parse_proxy_list("p:99999").empty());
    CHECK(parse_proxy_list("p:80x").empty());
}

TEST_CASE("system proxy: bypass lists as Windows reads them (<local>, wildcards, suffixes)") {
    const std::string list = "<local>;*.corp.example;10.*;.intra.example, gw.fairyfly.io:443 ; https://*.svc.example";
    CHECK(bypassed("intranet", 443, list));          // <local>: no dot
    CHECK(bypassed("SAP01.CORP.example", 443, list)); // without case
    CHECK(bypassed("10.1.2.3", 443, list));
    CHECK(bypassed("x.intra.example", 443, list));    // ".suffix"
    CHECK(bypassed("gw.fairyfly.io", 443, list));     // host:port
    CHECK_FALSE(bypassed("gw.fairyfly.io", 8443, list));
    CHECK(bypassed("a.svc.example", 443, list));      // scheme prefix ignored
    CHECK_FALSE(bypassed("fairyfly.io", 443, list));
    CHECK_FALSE(bypassed("corp.example.attacker.net", 443, list));
    CHECK_FALSE(bypassed("110.1.2.3", 443, list));
    CHECK_FALSE(bypassed("[fd00::1]", 443, "<local>"));  // an IPv6 literal is not a local name
    CHECK_FALSE(bypassed("fairyfly.io", 443, ""));
    CHECK(bypassed("anything.example", 443, "*"));
}

TEST_CASE("system proxy: the order of the settings - PAC/WPAD, then the manual proxy, then WinHTTP's machine setting") {
    const std::string url = "https://fairyfly.io/ffl";
    ProxySettings none;
    int calls = 0;
    auto counted = [&](AutoProxyResult r) { return [&calls, r] { ++calls; return r; }; };

    SECTION("nothing configured: DIRECT, and no script is evaluated") {
        auto r = resolve(none, std::nullopt, url, counted(pac("p:1")));
        CHECK(r.routes.empty());
        CHECK(calls == 0);
        CHECK(r.how == "no proxy configured");
    }
    SECTION("a PAC script decides, before the manual proxy") {
        ProxySettings s;
        s.auto_config_url = "http://wpad.corp.example/proxy.pac?token=abc";
        s.proxy = "manual:8080";
        auto r = resolve(s, std::nullopt, url, counted(pac("pac1:3128;pac2:3128")));
        CHECK(r.routes == Routes{via("pac1", 3128), via("pac2", 3128)});
        CHECK(r.how == "PAC http://wpad.corp.example/proxy.pac");  // the script address without its query
    }
    SECTION("the script says DIRECT") {
        ProxySettings s;
        s.auto_detect = true;
        AutoProxyResult d;
        d.ok = true;
        d.direct = true;
        auto r = resolve(s, std::nullopt, url, counted(d));
        CHECK(r.routes.empty());
        CHECK(r.how == "automatic detection (WPAD): DIRECT");
    }
    SECTION("the script's bypass list") {
        ProxySettings s;
        s.auto_detect = true;
        CHECK(resolve(s, std::nullopt, url, counted(pac("p:1", "*.fairyfly.io;fairyfly.io"))).routes.empty());
    }
    SECTION("automatic detection fails: the manual proxy, as Windows does") {
        ProxySettings s;
        s.auto_detect = true;
        s.proxy = "manual:8080";
        AutoProxyResult failed;
        failed.error = "no proxy configuration found by automatic detection";
        auto r = resolve(s, std::nullopt, url, counted(failed));
        CHECK(r.routes == Routes{via("manual", 8080)});
        CHECK(r.how == "automatic detection (WPAD) failed (no proxy configuration found by automatic detection), manual proxy settings");
    }
    SECTION("automatic detection fails and nothing else is set: DIRECT, saying why") {
        ProxySettings s;
        s.auto_detect = true;
        AutoProxyResult failed;
        failed.error = "timeout";
        auto r = resolve(s, std::nullopt, url, counted(failed));
        CHECK(r.routes.empty());
        CHECK(r.how == "automatic detection (WPAD) failed (timeout), no proxy configured");
    }
    SECTION("the manual proxy with \"bypass for local addresses\" and an exception") {
        ProxySettings s;
        s.proxy = "http=web:80;https=secure:8443";
        s.bypass = "<local>;*.fairyfly.io";
        CHECK(resolve(s, std::nullopt, "https://sap01/x", {}).routes.empty());
        CHECK(resolve(s, std::nullopt, "https://link.fairyfly.io/ffl", {}).routes.empty());
        CHECK(resolve(s, std::nullopt, "https://github.com/", {}).routes == Routes{via("secure", 8443)});
    }
    SECTION("WinHTTP's machine setting only when the user has none") {
        ProxySettings m;
        m.proxy = "machine:3128";
        m.bypass = "*.internal";
        CHECK(resolve(none, m, url, {}).routes == Routes{via("machine", 3128)});
        CHECK(resolve(none, m, "https://a.internal/", {}).routes.empty());
        ProxySettings user;
        user.proxy = "user:8080";
        CHECK(resolve(user, m, url, {}).routes == Routes{via("user", 8080)});
    }
}

TEST_CASE("system proxy: Negotiate before NTLM; once chosen, only that scheme's next challenge") {
    CHECK(pick_sspi_challenge({"Basic realm=\"corp\"", "NTLM", "Negotiate"}, "")->scheme == "Negotiate");
    CHECK(pick_sspi_challenge({"NTLM", "Basic realm=x"}, "")->scheme == "NTLM");
    CHECK_FALSE(pick_sspi_challenge({"Basic realm=x", "Digest realm=y"}, ""));
    auto next = pick_sspi_challenge({"NTLM TlRMTVNTUAACAAAA"}, "NTLM");
    REQUIRE(next);
    CHECK(next->token == "TlRMTVNTUAACAAAA");
    CHECK(pick_sspi_challenge({"negotiate  YIIGhg=="}, "Negotiate")->token == "YIIGhg==");
    CHECK(pick_sspi_challenge({"NTLM"}, "NTLM")->token.empty());  // restarted or rejected: the authenticator gives up
    CHECK_FALSE(pick_sspi_challenge({"Negotiate abc"}, "NTLM"));
    CHECK(proxy_spn("proxy.corp.example") == L"HTTP/proxy.corp.example");
    CHECK(proxy_spn("[fd00::1]") == L"HTTP/fd00::1");
}

#ifdef _WIN32
namespace {
// The proxy's side of the handshake with this user's own login (AcceptSecurityContext): what a Windows proxy (or
// IIS, ISA/TMG) does. Returns the challenges to send next, "" when the login is accepted.
class SspiServer {
public:
    explicit SspiServer(const wchar_t* package) : package_(package) {
        TimeStamp t{};
        REQUIRE(AcquireCredentialsHandleW(nullptr, const_cast<wchar_t*>(package), SECPKG_CRED_INBOUND, nullptr, nullptr, nullptr, nullptr,
                                          &cred_, &t) == SEC_E_OK);
    }
    ~SspiServer() {
        if (have_ctx_) DeleteSecurityContext(&ctx_);
        FreeCredentialsHandle(&cred_);
    }
    // header: the client's Proxy-Authorization ("NTLM <b64>"); returns the next Proxy-Authenticate, or "OK"
    std::string accept(const std::string& header) {
        auto sp = header.find(' ');
        REQUIRE(sp != std::string::npos);
        std::string scheme = header.substr(0, sp), in = ffy::b64std_decode(header.substr(sp + 1));
        SecBuffer ib{static_cast<unsigned long>(in.size()), SECBUFFER_TOKEN, in.data()};
        SecBufferDesc id{SECBUFFER_VERSION, 1, &ib};
        SecBuffer ob{0, SECBUFFER_TOKEN, nullptr};
        SecBufferDesc od{SECBUFFER_VERSION, 1, &ob};
        unsigned long attrs = 0;
        TimeStamp t{};
        SECURITY_STATUS st = AcceptSecurityContext(&cred_, have_ctx_ ? &ctx_ : nullptr, &id, ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_CONNECTION,
                                                   SECURITY_NATIVE_DREP, &ctx_, &od, &attrs, &t);
        have_ctx_ = true;
        std::string out(ob.pvBuffer ? std::string(static_cast<char*>(ob.pvBuffer), ob.cbBuffer) : std::string());
        if (ob.pvBuffer) FreeContextBuffer(ob.pvBuffer);
        if (st == SEC_E_OK) return "OK";
        REQUIRE(st == SEC_I_CONTINUE_NEEDED);
        return scheme + " " + ffy::b64std_encode(out);
    }

private:
    const wchar_t* package_;
    CredHandle cred_{};
    CtxtHandle ctx_{};
    bool have_ctx_ = false;
};
}  // namespace

TEST_CASE("system proxy (Windows): the SSPI authenticator logs in with this user's Windows login - NTLM, three legs on one connection") {
    SspiServer proxy(L"NTLM");
    auto a = make_sspi_authenticator("localhost");
    REQUIRE(a);
    auto t1 = a->respond({"Basic realm=\"corp\"", "NTLM"});  // what a proxy offers on its first 407
    REQUIRE(t1);
    CHECK(t1->rfind("NTLM ", 0) == 0);
    CHECK(a->scheme() == "NTLM");
    CHECK(ffy::b64std_decode(t1->substr(5)).rfind(std::string("NTLMSSP\0\1", 9), 0) == 0);  // Type 1
    auto challenge = proxy.accept(*t1);
    REQUIRE(challenge.rfind("NTLM ", 0) == 0);  // Type 2
    auto t3 = a->respond({challenge});
    REQUIRE(t3);
    CHECK(proxy.accept(*t3) == "OK");  // the login was accepted
    CHECK_FALSE(a->respond({"NTLM"}));  // another 407 after that: refused, the authenticator gives up
}

TEST_CASE("system proxy (Windows): Negotiate is preferred and completes against a Negotiate proxy") {
    SspiServer proxy(L"Negotiate");
    auto a = make_sspi_authenticator("localhost");
    auto tok = a->respond({"NTLM", "Negotiate"});
    REQUIRE(tok);
    CHECK(a->scheme() == "Negotiate");
    std::string next = proxy.accept(*tok);
    for (int leg = 0; leg < 4 && next != "OK"; ++leg) {
        auto t = a->respond({next});
        REQUIRE(t);
        next = proxy.accept(*t);
    }
    CHECK(next == "OK");
}

TEST_CASE("system proxy (Windows): the Windows resolver is installed for libffl and answers for this machine's settings") {
    install_system_proxy();
    const char* env = std::getenv("HTTPS_PROXY");
    auto d = ffy::proxy::decide("wss://link.fairyfly.io/ffl");  // the real WinHTTP calls; must not throw
    if (!env || !*env) CHECK(d.source == "Windows proxy settings");
    CHECK(d.authenticators.size() >= 2);  // SSPI, then a stored Basic credential
    // loopback never asks Windows
    CHECK(ffy::proxy::decide("wss://127.0.0.1:9/ffl").routes.empty());
}
#endif
