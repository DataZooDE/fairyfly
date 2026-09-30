#include "include/mcp/http_sys_mapping.h"

#include <algorithm>
#include <cctype>
#include <cstring>

namespace fairyfly::mcp::httpsys {

namespace {

std::string lower(std::string_view s) {
    std::string out(s);
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return out;
}

std::string_view trim(std::string_view s) {
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
    while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
    return s;
}

void fail(std::string* error, const std::string& message) {
    if (error) *error = message;
}

// HTTP_HEADER_ID order (http.h). Ids 0..19 are shared by requests and responses.
constexpr const char* kGeneralNames[20] = {
    "Cache-Control", "Connection", "Date", "Keep-Alive", "Pragma", "Trailer", "Transfer-Encoding", "Upgrade",
    "Via", "Warning", "Allow", "Content-Length", "Content-Type", "Content-Encoding", "Content-Language",
    "Content-Location", "Content-MD5", "Content-Range", "Expires", "Last-Modified"};
constexpr const char* kRequestOnlyNames[21] = {
    "Accept", "Accept-Charset", "Accept-Encoding", "Accept-Language", "Authorization", "Cookie", "Expect", "From",
    "Host", "If-Match", "If-Modified-Since", "If-None-Match", "If-Range", "If-Unmodified-Since", "Max-Forwards",
    "Proxy-Authorization", "Referer", "Range", "TE", "Translate", "User-Agent"};
constexpr const char* kResponseOnlyNames[10] = {"Accept-Ranges", "Age", "ETag", "Location", "Proxy-Authenticate",
                                                "Retry-After", "Server", "Set-Cookie", "Vary", "WWW-Authenticate"};

static_assert(HttpHeaderCacheControl == 0 && HttpHeaderLastModified == 19, "general header ids");
static_assert(HttpHeaderAccept == 20 && HttpHeaderHost == 28 && HttpHeaderUserAgent == 40, "request header ids");
static_assert(HttpHeaderRequestMaximum == 41 && HttpHeaderResponseMaximum == 30, "header id ranges");
static_assert(HttpHeaderAcceptRanges == 20 && HttpHeaderWwwAuthenticate == 29, "response header ids");

bool is_hop_by_hop(const std::string& lower_name) {
    static const char* const names[] = {"connection", "keep-alive", "proxy-connection", "te", "trailer",
                                        "transfer-encoding", "upgrade", "content-length"};
    for (const char* n : names)
        if (lower_name == n) return true;
    return false;
}

bool valid_host_char(char c) {
    return std::isalnum(static_cast<unsigned char>(c)) || c == '.' || c == '-' || c == '_' || c == ':' || c == '[' || c == ']';
}

} // namespace

bool is_loopback_host(std::string_view host) {
    const std::string h = lower(host);
    return h == "127.0.0.1" || h == "localhost" || h == "::1" || h == "[::1]";
}

std::string make_prefix(std::string_view scheme, std::string_view host_in, int port, std::string_view path,
                        std::string* error) {
    if (scheme != "http" && scheme != "https") {
        fail(error, "unsupported scheme '" + std::string(scheme) + "'");
        return {};
    }
    if (port == 0) {
        fail(error, "http.sys has no ephemeral ports: configure a fixed port");
        return {};
    }
    if (port < 1 || port > 65535) {
        fail(error, "port " + std::to_string(port) + " is outside 1..65535");
        return {};
    }
    if (path.empty() || path.front() != '/' || path.find_first_of("?#* \t\r\n") != std::string_view::npos) {
        fail(error, "invalid endpoint path '" + std::string(path) + "'");
        return {};
    }
    std::string host = lower(trim(host_in));
    if (host.empty() || host == "+" || host == "*" || host == "0.0.0.0") {
        host = "+";
    } else if (host == "localhost" || host == "127.0.0.1") {
        host = "127.0.0.1";
    } else if (host == "::1" || host == "[::1]") {
        host = "[::1]";
    } else {
        if (!std::all_of(host.begin(), host.end(), valid_host_char)) {
            fail(error, "invalid host '" + std::string(host_in) + "'");
            return {};
        }
        if (host.front() != '[' && host.find(':') != std::string::npos) host = "[" + host + "]";  // IPv6 literal
        if (host.front() == '[' && host.back() != ']') {
            fail(error, "invalid host '" + std::string(host_in) + "'");
            return {};
        }
    }
    std::string p(path);
    if (p.back() != '/') p += '/';
    return std::string(scheme) + "://" + host + ":" + std::to_string(port) + p;
}

std::string wide_to_utf8(const wchar_t* text, std::size_t chars) {
    if (!text || chars == 0) return {};
    const int needed = ::WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(chars), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return {};
    std::string out(static_cast<std::size_t>(needed), '\0');
    ::WideCharToMultiByte(CP_UTF8, 0, text, static_cast<int>(chars), out.data(), needed, nullptr, nullptr);
    return out;
}

std::wstring utf8_to_wide(std::string_view text) {
    if (text.empty()) return {};
    const int needed = ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (needed <= 0) return {};
    std::wstring out(static_cast<std::size_t>(needed), L'\0');
    ::MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), needed);
    return out;
}

std::string verb_to_string(const HTTP_REQUEST& request) {
    switch (request.Verb) {
    case HttpVerbUnparsed: return {};
    case HttpVerbUnknown:
        return request.pUnknownVerb ? std::string(request.pUnknownVerb, request.UnknownVerbLength) : std::string();
    case HttpVerbInvalid: return {};
    case HttpVerbOPTIONS: return "OPTIONS";
    case HttpVerbGET: return "GET";
    case HttpVerbHEAD: return "HEAD";
    case HttpVerbPOST: return "POST";
    case HttpVerbPUT: return "PUT";
    case HttpVerbDELETE: return "DELETE";
    case HttpVerbTRACE: return "TRACE";
    case HttpVerbCONNECT: return "CONNECT";
    case HttpVerbTRACK: return "TRACK";
    case HttpVerbMOVE: return "MOVE";
    case HttpVerbCOPY: return "COPY";
    case HttpVerbPROPFIND: return "PROPFIND";
    case HttpVerbPROPPATCH: return "PROPPATCH";
    case HttpVerbMKCOL: return "MKCOL";
    case HttpVerbLOCK: return "LOCK";
    case HttpVerbUNLOCK: return "UNLOCK";
    case HttpVerbSEARCH: return "SEARCH";
    default: return {};
    }
}

std::string request_path(const HTTP_REQUEST& request) {
    const HTTP_COOKED_URL& url = request.CookedUrl;
    if (!url.pAbsPath) return {};
    std::size_t chars = url.AbsPathLength / sizeof(wchar_t);
    // AbsPathLength already excludes the query string; cut at '?' anyway (never trust a hand-built struct).
    for (std::size_t i = 0; i < chars; ++i)
        if (url.pAbsPath[i] == L'?') {
            chars = i;
            break;
        }
    std::string path = wide_to_utf8(url.pAbsPath, chars);
    if (path.size() > 1 && path.back() == '/') path.pop_back();
    return path;
}

const char* request_header_name(int id) {
    if (id >= 0 && id < 20) return kGeneralNames[id];
    if (id >= 20 && id < HttpHeaderRequestMaximum) return kRequestOnlyNames[id - 20];
    return "";
}

HeaderMap request_headers(const HTTP_REQUEST& request) {
    HeaderMap out;
    const auto add = [&out](const std::string& name, const std::string& value) {
        auto it = out.find(name);
        if (it == out.end()) out.emplace(name, value);
        else it->second += ", " + value;
    };
    for (int id = 0; id < HttpHeaderRequestMaximum; ++id) {
        const HTTP_KNOWN_HEADER& h = request.Headers.KnownHeaders[id];
        if (h.RawValueLength == 0 || !h.pRawValue) continue;
        add(request_header_name(id), std::string(h.pRawValue, h.RawValueLength));
    }
    if (request.Headers.pUnknownHeaders) {
        for (USHORT i = 0; i < request.Headers.UnknownHeaderCount; ++i) {
            const HTTP_UNKNOWN_HEADER& h = request.Headers.pUnknownHeaders[i];
            if (!h.pName || h.NameLength == 0) continue;
            add(std::string(h.pName, h.NameLength), h.pRawValue ? std::string(h.pRawValue, h.RawValueLength) : std::string());
        }
    }
    return out;
}

std::string peer_address(const HTTP_REQUEST& request) {
    const SOCKADDR* sa = request.Address.pRemoteAddress;
    if (!sa) return {};
    char buf[INET6_ADDRSTRLEN + 1] = {};
    if (sa->sa_family == AF_INET) {
        const auto* in = reinterpret_cast<const SOCKADDR_IN*>(sa);
        if (!::inet_ntop(AF_INET, const_cast<IN_ADDR*>(&in->sin_addr), buf, sizeof(buf))) return {};
        return buf;
    }
    if (sa->sa_family == AF_INET6) {
        const auto* in6 = reinterpret_cast<const SOCKADDR_IN6*>(sa);
        const IN6_ADDR& a = in6->sin6_addr;
        if (IN6_IS_ADDR_V4MAPPED(&a)) {
            IN_ADDR v4;
            std::memcpy(&v4, &a.u.Byte[12], sizeof(v4));
            if (!::inet_ntop(AF_INET, &v4, buf, sizeof(buf))) return {};
            return buf;
        }
        if (!::inet_ntop(AF_INET6, const_cast<IN6_ADDR*>(&a), buf, sizeof(buf))) return {};
        return buf;
    }
    return {};
}

HttpRequest map_request(const HTTP_REQUEST& request) {
    HttpRequest out;
    out.method = verb_to_string(request);
    out.path = request_path(request);
    out.headers = request_headers(request);
    out.peer_addr = peer_address(request);
    out.tls = request.pSslInfo != nullptr;
    return out;
}

bool has_entity_body(const HTTP_REQUEST& request) {
    return (request.Flags & HTTP_REQUEST_FLAG_MORE_ENTITY_BODY_EXISTS) != 0;
}

bool is_http11(const HTTP_REQUEST& request) {
    return request.Version.MajorVersion == 1 && request.Version.MinorVersion >= 1;
}

ContentLength parse_content_length(std::string_view header_value) {
    ContentLength out;
    const std::string_view v = trim(header_value);
    if (header_value.empty()) return out;
    out.present = true;
    if (v.empty()) return out;
    std::uint64_t value = 0;
    for (char c : v) {
        if (c < '0' || c > '9') return out;
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (value > (UINT64_MAX - digit) / 10) return out;  // overflow
        value = value * 10 + digit;
    }
    out.valid = true;
    out.value = value;
    return out;
}

const char* status_text(int status) {
    switch (status) {
    case 100: return "Continue";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 301: return "Moved Permanently";
    case 302: return "Found";
    case 304: return "Not Modified";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 411: return "Length Required";
    case 413: return "Payload Too Large";
    case 415: return "Unsupported Media Type";
    case 429: return "Too Many Requests";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 502: return "Bad Gateway";
    case 503: return "Service Unavailable";
    case 504: return "Gateway Timeout";
    default: return "Unknown";
    }
}

int response_header_id(std::string_view name) {
    const std::string n = lower(name);
    for (int i = 0; i < 20; ++i)
        if (n == lower(kGeneralNames[i])) return i;
    for (int i = 0; i < 10; ++i)
        if (n == lower(kResponseOnlyNames[i])) return 20 + i;
    return -1;
}

PackedHeaders pack_response_headers(const HttpResponse& response, bool chunked) {
    PackedHeaders out;
    bool has_content_type = false;
    for (const auto& h : response.headers) {
        const std::string lname = lower(h.first);
        if (lname == "content-type") has_content_type = true;
        if (is_hop_by_hop(lname)) continue;
        const int id = response_header_id(h.first);
        if (id >= 0) {
            bool replaced = false;
            for (auto& k : out.known)
                if (k.first == id) {
                    k.second = h.second;
                    replaced = true;
                }
            if (!replaced) out.known.emplace_back(id, h.second);
        } else {
            out.unknown.emplace_back(h.first, h.second);
        }
    }
    if (!has_content_type && !response.body.empty()) out.known.emplace_back(HttpHeaderContentType, "application/json");
    if (chunked) out.known.emplace_back(HttpHeaderTransferEncoding, "chunked");
    return out;
}

} // namespace fairyfly::mcp::httpsys
