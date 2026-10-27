#pragma once
// The setup probes: one HTTP request (WinHTTP; certificate errors ignored, the presented certificate's SHA-1 and the
// negotiated TLS protocol reported so the CALLER pins the thumbprint; probes send no credentials, so ignoring chain
// errors before the pin check leaks nothing) and the logic around it, which takes the transport as a function.
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "include/setup/setup_hosts.h"

namespace fairyfly::setup {

struct HttpProbeRequest {
    std::string host;                 ///< name used for SNI and the Host header
    std::string connect_host;         ///< where to connect (empty = host)
    int port = 0;
    bool tls = true;
    std::string method = "GET";
    std::string path = "/mcp";
    std::vector<std::pair<std::string, std::string>> headers;
    std::string body;
    int timeout_ms = 5000;
};

struct HttpProbeResponse {
    bool ok = false;                  ///< an HTTP response was received
    unsigned long win_error = 0;
    std::string error;
    int status = 0;
    std::string www_authenticate;
    std::string body;
    std::string protocol;             ///< "TLS 1.2" ...
    std::string cert_thumbprint;      ///< SHA-1 of the presented certificate (TLS only)
};

/// One request: winhttp_request, or a fake in tests.
using HttpTransport = std::function<HttpProbeResponse(const HttpProbeRequest&)>;

/// The real request over WinHTTP (redirects are never followed).
HttpProbeResponse winhttp_request(const HttpProbeRequest& request);

/// The headers sent: a request connected elsewhere (connect_host) names the real host and port in Host first.
std::vector<std::pair<std::string, std::string>> probe_headers(const HttpProbeRequest& request);

/// mcp setup's verification request: an MCP initialize POSTed to /mcp of the host name (127.0.0.1 without TLS); a
/// name that does not resolve is retried on 127.0.0.1 with the real name (the pin is the thumbprint).
HttpProbeResponse verify_round_trip(const VerifyRequest& request, const HttpTransport& transport);
/// ok when the answer is 401 AUTH_REQUIRED with a WWW-Authenticate challenge and (TLS) the pinned certificate.
VerifyResult judge_verify(const HttpProbeResponse& response, const VerifyRequest& request);

/// The doctor's TLS probe of host:port against `expected_thumbprint`, with the same fallback for a name that does not
/// resolve, a port that refuses or a timeout. status: ok | unreachable | handshake_failed | error.
TlsProbe probe_tls(const std::string& host, int port, const std::string& expected_thumbprint, const HttpTransport& transport);

} // namespace fairyfly::setup
