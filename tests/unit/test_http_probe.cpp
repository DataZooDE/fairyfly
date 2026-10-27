// The setup probes' logic around one HTTP request (review 2026-10-25: the WinHTTP probe had no seam): which request
// is sent, the fallback to 127.0.0.1 with the real Host name, the verdict and the error mapping.
#include <catch2/catch_test_macros.hpp>

#include <winsock2.h>
#include <windows.h>
#include <winhttp.h>

#include <vector>

#include "include/setup/http_probe.h"

using namespace fairyfly::setup;

namespace {
struct FakeTransport {
    std::vector<HttpProbeRequest> sent;
    std::vector<HttpProbeResponse> answers;   // in order; the last one repeats
    HttpTransport transport() {
        return [this](const HttpProbeRequest& request) {
            sent.push_back(request);
            return answers[std::min(sent.size(), answers.size()) - 1];
        };
    }
};
HttpProbeResponse failed(unsigned long win_error) {
    HttpProbeResponse r;
    r.win_error = win_error;
    r.error = "error " + std::to_string(win_error);
    return r;
}
HttpProbeResponse answered(int status, const std::string& body, const std::string& challenge, const std::string& thumbprint) {
    HttpProbeResponse r;
    r.ok = true;
    r.status = status;
    r.body = body;
    r.www_authenticate = challenge;
    r.cert_thumbprint = thumbprint;
    r.protocol = "TLS 1.3";
    return r;
}
VerifyRequest tls_request() {
    VerifyRequest v;
    v.tls = true;
    v.hostname = "mcp.example.com";
    v.port = 8443;
    v.expected_thumbprint = "AB12";
    return v;
}
}  // namespace

TEST_CASE("http probe: verify posts an MCP initialize to the host name, or to 127.0.0.1 without TLS", "[setup][probe]") {
    FakeTransport fake;
    fake.answers = {answered(401, R"({"code":"AUTH_REQUIRED"})", "Bearer realm=\"fairyfly\"", "AB12")};
    (void)verify_round_trip(tls_request(), fake.transport());
    REQUIRE(fake.sent.size() == 1);
    CHECK(fake.sent[0].host == "mcp.example.com");
    CHECK(fake.sent[0].connect_host.empty());
    CHECK(fake.sent[0].tls);
    CHECK(fake.sent[0].method == "POST");
    CHECK(fake.sent[0].path == "/mcp");
    CHECK(fake.sent[0].body.find("\"initialize\"") != std::string::npos);
    VerifyRequest plain = tls_request();
    plain.tls = false;
    (void)verify_round_trip(plain, fake.transport());
    CHECK(fake.sent[1].host == "127.0.0.1");
    CHECK_FALSE(fake.sent[1].tls);
}

TEST_CASE("http probe: a host name that does not resolve is retried on 127.0.0.1 with the real name", "[setup][probe]") {
    FakeTransport fake;
    fake.answers = {failed(ERROR_WINHTTP_NAME_NOT_RESOLVED), answered(401, "AUTH_REQUIRED", "Bearer", "AB12")};
    const HttpProbeResponse r = verify_round_trip(tls_request(), fake.transport());
    REQUIRE(fake.sent.size() == 2);
    CHECK(fake.sent[1].connect_host == "127.0.0.1");
    CHECK(fake.sent[1].host == "mcp.example.com");   // SNI and Host keep the name; the pin is the thumbprint
    CHECK(r.ok);
    // the retry failing too reports the first error; other errors are not retried
    FakeTransport both;
    both.answers = {failed(ERROR_WINHTTP_NAME_NOT_RESOLVED), failed(ERROR_WINHTTP_CANNOT_CONNECT)};
    CHECK(verify_round_trip(tls_request(), both.transport()).win_error == ERROR_WINHTTP_NAME_NOT_RESOLVED);
    FakeTransport refused;
    refused.answers = {failed(ERROR_WINHTTP_CANNOT_CONNECT)};
    (void)verify_round_trip(tls_request(), refused.transport());
    CHECK(refused.sent.size() == 1);
}

TEST_CASE("http probe: the verdict wants 401 AUTH_REQUIRED, a challenge and the pinned certificate", "[setup][probe]") {
    const VerifyRequest v = tls_request();
    CHECK(judge_verify(answered(401, "AUTH_REQUIRED", "Bearer", "AB12"), v).status == "ok");
    const auto other_cert = judge_verify(answered(401, "AUTH_REQUIRED", "Bearer", "FFFF"), v);
    CHECK(other_cert.status == "failed");
    CHECK(other_cert.detail == "the server presented a different certificate (FFFF)");
    CHECK(judge_verify(answered(200, "{}", "", "AB12"), v).detail ==
          "expected 401 AUTH_REQUIRED for an unauthenticated request, got HTTP 200");
    CHECK(judge_verify(answered(401, "AUTH_REQUIRED", "", "AB12"), v).detail == "401 without a WWW-Authenticate header");
    const auto unresolved = judge_verify(failed(ERROR_WINHTTP_NAME_NOT_RESOLVED), v);
    CHECK(unresolved.status == "failed");
    CHECK(unresolved.detail.find("add a DNS record or hosts entry for mcp.example.com") != std::string::npos);
    VerifyRequest plain = v;
    plain.tls = false;
    CHECK(judge_verify(answered(401, "AUTH_REQUIRED", "Bearer", ""), plain).thumbprint_match);   // no pin without TLS
}

TEST_CASE("http probe: the TLS probe falls back to 127.0.0.1 and maps WinHTTP errors", "[setup][probe]") {
    for (unsigned long error : {ERROR_WINHTTP_NAME_NOT_RESOLVED, ERROR_WINHTTP_CANNOT_CONNECT, ERROR_WINHTTP_TIMEOUT}) {
        FakeTransport fake;
        fake.answers = {failed(error), answered(401, "", "", "AB12")};
        const TlsProbe p = probe_tls("mcp.example.com", 8443, "AB12", fake.transport());
        REQUIRE(fake.sent.size() == 2);
        CHECK(fake.sent[1].connect_host == "127.0.0.1");
        CHECK(p.status == "ok");
        CHECK(p.thumbprint_match);
        CHECK(p.protocol == "TLS 1.3");
    }
    FakeTransport local;
    local.answers = {failed(ERROR_WINHTTP_CANNOT_CONNECT)};
    CHECK(probe_tls("127.0.0.1", 8443, "AB12", local.transport()).status == "unreachable");
    CHECK(local.sent.size() == 1);   // already local: no second try
    FakeTransport handshake;
    handshake.answers = {failed(ERROR_WINHTTP_SECURE_FAILURE)};
    CHECK(probe_tls("mcp.example.com", 8443, "AB12", handshake.transport()).status == "handshake_failed");
    FakeTransport other;
    other.answers = {failed(ERROR_WINHTTP_INTERNAL_ERROR)};
    CHECK(probe_tls("mcp.example.com", 8443, "AB12", other.transport()).status == "error");
    FakeTransport no_pin;
    no_pin.answers = {answered(401, "", "", "AB12")};
    CHECK_FALSE(probe_tls("mcp.example.com", 8443, "", no_pin.transport()).thumbprint_match);
}

TEST_CASE("http probe: a request sent elsewhere names the real host in its Host header", "[setup][probe]") {
    HttpProbeRequest request;
    request.host = "mcp.example.com";
    request.port = 8443;
    request.headers = {{"Accept", "application/json"}};
    CHECK(probe_headers(request) == std::vector<std::pair<std::string, std::string>>{{"Accept", "application/json"}});
    request.connect_host = "127.0.0.1";
    CHECK(probe_headers(request) ==
          std::vector<std::pair<std::string, std::string>>{{"Host", "mcp.example.com:8443"}, {"Accept", "application/json"}});
}
