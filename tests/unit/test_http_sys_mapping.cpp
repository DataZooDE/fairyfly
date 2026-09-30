// Pure http.sys <-> HttpRequest/HttpResponse mapping over hand-built HTTP_REQUEST structs. No sockets, no kernel.
#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>
#include <vector>

#include "include/mcp/http_sys_mapping.h"

using namespace fairyfly::mcp;
using namespace fairyfly::mcp::httpsys;

namespace {

/// Owns the storage a hand-built HTTP_REQUEST points into.
struct Built {
    HTTP_REQUEST req{};
    std::wstring url_path;
    std::vector<std::string> strings;  // stable storage (reserved up front, never reallocated)
    std::vector<HTTP_UNKNOWN_HEADER> unknown;
    SOCKADDR_IN v4{};
    SOCKADDR_IN6 v6{};

    Built() { strings.reserve(64); unknown.reserve(16); }

    const char* keep(const std::string& s) {
        strings.push_back(s);
        return strings.back().c_str();
    }
    void path(const std::wstring& p, std::size_t abs_chars = std::wstring::npos) {
        url_path = p;
        req.CookedUrl.pAbsPath = url_path.data();
        req.CookedUrl.AbsPathLength = static_cast<USHORT>((abs_chars == std::wstring::npos ? p.size() : abs_chars) * sizeof(wchar_t));
    }
    void known(HTTP_HEADER_ID id, const std::string& value) {
        req.Headers.KnownHeaders[id].pRawValue = keep(value);
        req.Headers.KnownHeaders[id].RawValueLength = static_cast<USHORT>(value.size());
    }
    void add_unknown(const std::string& name, const std::string& value) {
        HTTP_UNKNOWN_HEADER h{};
        h.pName = keep(name);
        h.NameLength = static_cast<USHORT>(name.size());
        h.pRawValue = keep(value);
        h.RawValueLength = static_cast<USHORT>(value.size());
        unknown.push_back(h);
        req.Headers.pUnknownHeaders = unknown.data();
        req.Headers.UnknownHeaderCount = static_cast<USHORT>(unknown.size());
    }
    void peer4(const char* ip, unsigned short port = 50000) {
        v4.sin_family = AF_INET;
        v4.sin_port = htons(port);
        ::inet_pton(AF_INET, ip, &v4.sin_addr);
        req.Address.pRemoteAddress = reinterpret_cast<PSOCKADDR>(&v4);
    }
    void peer6(const char* ip, unsigned short port = 50000) {
        v6.sin6_family = AF_INET6;
        v6.sin6_port = htons(port);
        ::inet_pton(AF_INET6, ip, &v6.sin6_addr);
        req.Address.pRemoteAddress = reinterpret_cast<PSOCKADDR>(&v6);
    }
};

} // namespace

TEST_CASE("verbs map to method names", "[httpsys][mapping]") {
    Built b;
    struct Case { HTTP_VERB verb; const char* name; };
    for (const Case& c : {Case{HttpVerbGET, "GET"}, Case{HttpVerbPOST, "POST"}, Case{HttpVerbPUT, "PUT"},
                          Case{HttpVerbDELETE, "DELETE"}, Case{HttpVerbHEAD, "HEAD"}, Case{HttpVerbOPTIONS, "OPTIONS"},
                          Case{HttpVerbTRACE, "TRACE"}, Case{HttpVerbCONNECT, "CONNECT"}}) {
        b.req.Verb = c.verb;
        CHECK(verb_to_string(b.req) == c.name);
    }
    b.req.Verb = HttpVerbUnknown;
    b.req.pUnknownVerb = "PATCH";
    b.req.UnknownVerbLength = 5;
    CHECK(verb_to_string(b.req) == "PATCH");
    b.req.Verb = HttpVerbUnparsed;
    CHECK(verb_to_string(b.req).empty());
}

TEST_CASE("path: cooked, query cut, trailing slash stripped, UTF-8", "[httpsys][mapping]") {
    Built b;
    b.path(L"/mcp/");
    CHECK(request_path(b.req) == "/mcp");
    b.path(L"/mcp");
    CHECK(request_path(b.req) == "/mcp");
    b.path(L"/");
    CHECK(request_path(b.req) == "/");
    // AbsPathLength already excludes the query, and a '?' inside the buffer is cut as well.
    b.path(L"/mcp/?a=1&b=2", 5);
    CHECK(request_path(b.req) == "/mcp");
    b.path(L"/mcp?x=/");
    CHECK(request_path(b.req) == "/mcp");
    // http.sys canonicalises %6D to m before we see it; a literal encoded slash stays untouched text.
    b.path(L"/mcp/a%2Fb");
    CHECK(request_path(b.req) == "/mcp/a%2Fb");
    b.path(L"/café/");
    CHECK(request_path(b.req) == "/caf\xC3\xA9");
    b.path(L"/mcp/x//");  // only ONE trailing slash is stripped
    CHECK(request_path(b.req) == "/mcp/x/");
    Built empty;
    CHECK(request_path(empty.req).empty());
}

TEST_CASE("headers: known, unknown, duplicates", "[httpsys][mapping]") {
    Built b;
    b.known(HttpHeaderHost, "example.org:8443");
    b.known(HttpHeaderContentType, "application/json");
    b.known(HttpHeaderAuthorization, "Bearer abc");
    b.known(HttpHeaderContentLength, "12");
    b.add_unknown("X-Custom", "one");
    b.add_unknown("x-custom", "two");
    b.add_unknown("Mcp-Method", "tools/list");
    const HeaderMap h = request_headers(b.req);
    CHECK(h.at("host") == "example.org:8443");
    CHECK(h.at("Content-Type") == "application/json");
    CHECK(h.at("AUTHORIZATION") == "Bearer abc");
    CHECK(h.at("content-length") == "12");
    CHECK(h.at("X-Custom") == "one, two");  // duplicates joined like the previous adapter
    CHECK(h.at("mcp-method") == "tools/list");
    CHECK(h.count("Accept") == 0);
    CHECK(std::string(request_header_name(HttpHeaderHost)) == "Host");
    CHECK(std::string(request_header_name(HttpHeaderUserAgent)) == "User-Agent");
    CHECK(std::string(request_header_name(HttpHeaderAccept)) == "Accept");
    CHECK(std::string(request_header_name(HttpHeaderContentLength)) == "Content-Length");
    CHECK(std::string(request_header_name(HttpHeaderTe)) == "TE");
    CHECK(std::string(request_header_name(99)).empty());
}

TEST_CASE("peer address: IPv4, IPv6 and v4-mapped", "[httpsys][mapping]") {
    Built a;
    a.peer4("192.168.1.20");
    CHECK(peer_address(a.req) == "192.168.1.20");
    Built b;
    b.peer6("fe80::1234");
    CHECK(peer_address(b.req) == "fe80::1234");
    Built c;
    c.peer6("::1");
    CHECK(peer_address(c.req) == "::1");
    Built d;
    d.peer6("::ffff:10.1.2.3");
    CHECK(peer_address(d.req) == "10.1.2.3");
    Built none;
    CHECK(peer_address(none.req).empty());
}

TEST_CASE("map_request: tls flag, body flag, HTTP version", "[httpsys][mapping]") {
    Built b;
    b.req.Verb = HttpVerbPOST;
    b.path(L"/mcp/");
    b.known(HttpHeaderHost, "127.0.0.1:18383");
    b.peer4("127.0.0.1");
    HttpRequest plain = map_request(b.req);
    CHECK(plain.method == "POST");
    CHECK(plain.path == "/mcp");
    CHECK(plain.peer_addr == "127.0.0.1");
    CHECK(plain.header("Host") == "127.0.0.1:18383");
    CHECK_FALSE(plain.tls);
    CHECK(plain.body.empty());

    HTTP_SSL_INFO ssl{};
    b.req.pSslInfo = &ssl;
    CHECK(map_request(b.req).tls);

    CHECK_FALSE(has_entity_body(b.req));
    b.req.Flags = HTTP_REQUEST_FLAG_MORE_ENTITY_BODY_EXISTS;
    CHECK(has_entity_body(b.req));

    b.req.Version.MajorVersion = 1;
    b.req.Version.MinorVersion = 0;
    CHECK_FALSE(is_http11(b.req));
    b.req.Version.MinorVersion = 1;
    CHECK(is_http11(b.req));
    b.req.Version.MajorVersion = 2;
    b.req.Version.MinorVersion = 0;
    CHECK_FALSE(is_http11(b.req));  // HTTP/2: no chunked framing by the application
}

TEST_CASE("Content-Length parsing edge cases", "[httpsys][mapping]") {
    CHECK_FALSE(parse_content_length("").present);
    auto v = parse_content_length("0");
    CHECK((v.present && v.valid && v.value == 0));
    v = parse_content_length(" 1234 ");
    CHECK((v.valid && v.value == 1234));
    v = parse_content_length("18446744073709551615");
    CHECK((v.valid && v.value == UINT64_MAX));
    for (const char* bad : {"18446744073709551616", "-1", "+5", "12a", "1 2", "1, 1", "0x10", " "}) {
        const auto r = parse_content_length(bad);
        INFO(bad);
        CHECK(r.present);
        CHECK_FALSE(r.valid);
    }
}

TEST_CASE("prefix builder", "[httpsys][mapping]") {
    std::string err;
    CHECK(make_prefix("https", "+", 8443, "/mcp", &err) == "https://+:8443/mcp/");
    CHECK(make_prefix("https", "*", 8443, "/mcp") == "https://+:8443/mcp/");
    CHECK(make_prefix("https", "", 8443, "/mcp") == "https://+:8443/mcp/");
    CHECK(make_prefix("https", "sap.example.com", 8443, "/mcp") == "https://sap.example.com:8443/mcp/");
    CHECK(make_prefix("https", "SAP.Example.COM", 443, "/mcp/") == "https://sap.example.com:443/mcp/");
    CHECK(make_prefix("http", "127.0.0.1", 18383, "/mcp") == "http://127.0.0.1:18383/mcp/");
    CHECK(make_prefix("http", "localhost", 18383, "/mcp") == "http://127.0.0.1:18383/mcp/");
    CHECK(make_prefix("http", "LocalHost", 18383, "/mcp") == "http://127.0.0.1:18383/mcp/");
    CHECK(make_prefix("http", "::1", 18383, "/mcp") == "http://[::1]:18383/mcp/");
    CHECK(make_prefix("http", "[::1]", 18383, "/mcp") == "http://[::1]:18383/mcp/");
    CHECK(make_prefix("https", "fe80::1", 8443, "/mcp") == "https://[fe80::1]:8443/mcp/");
    CHECK(make_prefix("http", "0.0.0.0", 8383, "/mcp") == "http://+:8383/mcp/");

    for (const auto& c : std::vector<std::tuple<std::string, std::string, int, std::string>>{
             {"ftp", "127.0.0.1", 80, "/mcp"},   // scheme
             {"http", "127.0.0.1", 0, "/mcp"},     // no ephemeral ports
             {"http", "127.0.0.1", -1, "/mcp"},
             {"http", "127.0.0.1", 65536, "/mcp"},
             {"http", "bad host", 8383, "/mcp"},   // host chars
             {"http", "a/b", 8383, "/mcp"},
             {"http", "[::1", 8383, "/mcp"},
             {"http", "127.0.0.1", 8383, "mcp"},   // path must start with '/'
             {"http", "127.0.0.1", 8383, ""},
             {"http", "127.0.0.1", 8383, "/m cp"},
             {"http", "127.0.0.1", 8383, "/mcp?x"}}) {
        std::string e;
        INFO(std::get<1>(c) << ":" << std::get<2>(c) << std::get<3>(c));
        CHECK(make_prefix(std::get<0>(c), std::get<1>(c), std::get<2>(c), std::get<3>(c), &e).empty());
        CHECK_FALSE(e.empty());
    }
    std::string e;
    make_prefix("http", "127.0.0.1", 0, "/mcp", &e);
    CHECK(e.find("ephemeral") != std::string::npos);

    CHECK(is_loopback_host("127.0.0.1"));
    CHECK(is_loopback_host("localhost"));
    CHECK(is_loopback_host("::1"));
    CHECK(is_loopback_host("[::1]"));
    CHECK_FALSE(is_loopback_host("0.0.0.0"));
    CHECK_FALSE(is_loopback_host("10.0.0.5"));
    CHECK_FALSE(is_loopback_host("+"));
}

TEST_CASE("UTF conversions round-trip", "[httpsys][mapping]") {
    CHECK(wide_to_utf8(L"abc", 3) == "abc");
    CHECK(wide_to_utf8(L"ä", 1) == "\xC3\xA4");
    CHECK(utf8_to_wide("\xC3\xA4") == L"ä");
    CHECK(wide_to_utf8(nullptr, 3).empty());
}

TEST_CASE("response header packing", "[httpsys][mapping]") {
    HttpResponse r;
    r.status = 401;
    r.body = "{}";
    r.set_header("WWW-Authenticate", "Bearer realm=\"x\"");
    r.set_header("Cache-Control", "no-store");
    r.set_header("X-Content-Type-Options", "nosniff");
    r.set_header("Connection", "keep-alive");          // hop-by-hop: dropped
    r.set_header("Transfer-Encoding", "gzip");         // dropped
    r.set_header("Content-Length", "999");             // dropped: http.sys frames the body
    r.set_header("Retry-After", "1");

    PackedHeaders p = pack_response_headers(r, false);
    const auto find_known = [&](int id) -> const std::string* {
        for (const auto& k : p.known)
            if (k.first == id) return &k.second;
        return nullptr;
    };
    REQUIRE(find_known(HttpHeaderWwwAuthenticate));
    CHECK(*find_known(HttpHeaderWwwAuthenticate) == "Bearer realm=\"x\"");
    CHECK(*find_known(HttpHeaderCacheControl) == "no-store");
    CHECK(*find_known(HttpHeaderRetryAfter) == "1");
    REQUIRE(find_known(HttpHeaderContentType));  // defaulted because there is a body
    CHECK(*find_known(HttpHeaderContentType) == "application/json");
    CHECK(find_known(HttpHeaderConnection) == nullptr);
    CHECK(find_known(HttpHeaderTransferEncoding) == nullptr);
    CHECK(find_known(HttpHeaderContentLength) == nullptr);
    REQUIRE(p.unknown.size() == 1);
    CHECK(p.unknown[0].first == "X-Content-Type-Options");

    // an explicit Content-Type wins, and an empty body gets no default
    HttpResponse s;
    s.status = 202;
    p = pack_response_headers(s, false);
    CHECK(find_known(HttpHeaderContentType) == nullptr);
    s.set_header("content-type", "text/event-stream");
    p = pack_response_headers(s, true);
    CHECK(*find_known(HttpHeaderContentType) == "text/event-stream");
    REQUIRE(find_known(HttpHeaderTransferEncoding));
    CHECK(*find_known(HttpHeaderTransferEncoding) == "chunked");

    CHECK(response_header_id("www-authenticate") == HttpHeaderWwwAuthenticate);
    CHECK(response_header_id("ETAG") == HttpHeaderEtag);
    CHECK(response_header_id("X-Nope") == -1);
    CHECK(response_header_id("Accept") == -1);  // request-only
}

TEST_CASE("status text", "[httpsys][mapping]") {
    CHECK(std::string(status_text(200)) == "OK");
    CHECK(std::string(status_text(401)) == "Unauthorized");
    CHECK(std::string(status_text(413)) == "Payload Too Large");
    CHECK(std::string(status_text(503)) == "Service Unavailable");
    CHECK(std::string(status_text(299)) == "Unknown");
}
