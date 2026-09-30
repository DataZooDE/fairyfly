#pragma once
// Pure mapping between http.sys structures (HTTP_REQUEST / HTTP_RESPONSE) and the transport-neutral
// HttpRequest / HttpResponse of http_endpoint.h. No handles, no I/O: unit-tested over hand-built structs.

// Order matters: winsock2 and ws2tcpip before windows.h before http.h.
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <http.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "include/mcp/http_endpoint.h"

namespace fairyfly::mcp::httpsys {

/// True for 127.0.0.1, localhost, ::1 and [::1] (case-insensitive).
bool is_loopback_host(std::string_view host);

/// http.sys URL prefix for `scheme` ("http" | "https"), host, port and endpoint path: "https://+:8443/mcp/".
/// Wildcard hosts ("", "+", "*", "0.0.0.0") become "+", "localhost" becomes 127.0.0.1, "::1" becomes "[::1]",
/// other IPv6 literals get brackets. The trailing slash is mandatory for http.sys and always added.
/// Returns "" and sets *error on invalid input (bad scheme, port outside 1..65535 incl. 0, bad host or path).
std::string make_prefix(std::string_view scheme, std::string_view host, int port, std::string_view path,
                        std::string* error = nullptr);

/// UTF-16 -> UTF-8 (invalid sequences become U+FFFD).
std::string wide_to_utf8(const wchar_t* text, std::size_t chars);
/// UTF-8 -> UTF-16.
std::wstring utf8_to_wide(std::string_view text);

/// HTTP_VERB -> method name; HttpVerbUnknown -> pUnknownVerb (UnknownVerbLength bytes).
std::string verb_to_string(const HTTP_REQUEST& request);
/// Cooked absolute path (query removed, UTF-8, one trailing slash stripped unless the path is "/").
std::string request_path(const HTTP_REQUEST& request);
/// Known + unknown request headers; duplicates are joined with ", ".
HeaderMap request_headers(const HTTP_REQUEST& request);
/// Canonical text of the remote address (IPv4 dotted, IPv6 textual; IPv4-mapped IPv6 as dotted IPv4). "" if unknown.
std::string peer_address(const HTTP_REQUEST& request);
/// Everything except the body: method, path, headers, peer_addr, tls (pSslInfo != nullptr).
HttpRequest map_request(const HTTP_REQUEST& request);
/// The request announces a body (HTTP_REQUEST_FLAG_MORE_ENTITY_BODY_EXISTS).
bool has_entity_body(const HTTP_REQUEST& request);
/// Exactly HTTP/1.x with minor >= 1: streamed responses need explicit chunked framing (HTTP/2 frames DATA itself).
bool is_http11(const HTTP_REQUEST& request);

struct ContentLength {
    bool present = false;   ///< a Content-Length header exists
    bool valid = false;     ///< present and a plain decimal number that fits into 64 bits
    std::uint64_t value = 0;
};
ContentLength parse_content_length(std::string_view header_value);

/// Reason phrase of a status code ("OK", "Unauthorized", ...); "Unknown" for unlisted codes.
const char* status_text(int status);

/// HTTP_HEADER_ID of a response header name (case-insensitive) or -1 when it is not a known response header.
int response_header_id(std::string_view name);
/// Name of a known request header id (HttpHeaderHost -> "Host"); "" for out-of-range ids.
const char* request_header_name(int id);

struct PackedHeaders {
    std::vector<std::pair<int, std::string>> known;                   ///< (HTTP_HEADER_ID, value)
    std::vector<std::pair<std::string, std::string>> unknown;         ///< everything else
};
/// Headers of `response` for HttpSendHttpResponse: hop-by-hop headers and Content-Length are dropped (http.sys
/// frames the message), Content-Type defaults to application/json when there is a body. `chunked` adds
/// "Transfer-Encoding: chunked" (streamed responses on HTTP/1.1).
PackedHeaders pack_response_headers(const HttpResponse& response, bool chunked);

} // namespace fairyfly::mcp::httpsys
