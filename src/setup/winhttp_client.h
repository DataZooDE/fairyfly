#pragma once
// Minimal WinHTTP client for the setup probes (internal to src/setup): one request, certificate errors ignored, the
// presented certificate's SHA-1 and the negotiated TLS protocol reported so the CALLER pins the thumbprint. Probes send
// no credentials, so ignoring chain errors before the pin check leaks nothing.
#include <string>
#include <utility>
#include <vector>

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

HttpProbeResponse winhttp_request(const HttpProbeRequest& request);

} // namespace fairyfly::setup
