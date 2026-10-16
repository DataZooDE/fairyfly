#include "util.hpp"

#include <openssl/rand.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>

namespace ffy {

static const char* kUrl = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
static const char* kStd = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string b64_encode(std::string_view in, const char* tbl, bool pad) {
    std::string out;
    out.reserve((in.size() + 2) / 3 * 4);
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3) {
        uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8) | uint8_t(in[i + 2]);
        out += tbl[v >> 18];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        out += tbl[v & 63];
    }
    if (i + 1 == in.size()) {
        uint32_t v = uint8_t(in[i]) << 16;
        out += tbl[v >> 18];
        out += tbl[(v >> 12) & 63];
        if (pad) out += "==";
    } else if (i + 2 == in.size()) {
        uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8);
        out += tbl[v >> 18];
        out += tbl[(v >> 12) & 63];
        out += tbl[(v >> 6) & 63];
        if (pad) out += '=';
    }
    return out;
}

static std::string b64_decode(std::string_view in, bool url) {
    int8_t rev[256];
    std::fill(rev, rev + 256, int8_t(-1));
    const char* tbl = url ? kUrl : kStd;
    for (int i = 0; i < 64; i++) rev[uint8_t(tbl[i])] = int8_t(i);
    while (!in.empty() && in.back() == '=') in.remove_suffix(1);
    std::string out;
    uint32_t acc = 0;
    int bits = 0;
    for (char c : in) {
        int8_t v = rev[uint8_t(c)];
        if (v < 0) throw Error("base64: bad character");
        acc = (acc << 6) | uint32_t(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out += char((acc >> bits) & 0xff);
        }
    }
    if (bits >= 6) throw Error("base64: truncated input");
    return out;
}

std::string b64url_encode(std::string_view in) { return b64_encode(in, kUrl, false); }
std::string b64url_decode(std::string_view in) { return b64_decode(in, true); }
std::string b64std_encode(std::string_view in) { return b64_encode(in, kStd, true); }
std::string b64std_decode(std::string_view in) { return b64_decode(in, false); }

std::string hex_encode(std::string_view in) {
    static const char* d = "0123456789abcdef";
    std::string o;
    for (unsigned char c : in) {
        o += d[c >> 4];
        o += d[c & 15];
    }
    return o;
}

std::string rand_bytes(size_t n) {
    std::string b(n, '\0');
    if (RAND_bytes(reinterpret_cast<unsigned char*>(b.data()), int(n)) != 1) throw Error("RAND_bytes failed");
    return b;
}
std::string rand_hex(size_t n) { return hex_encode(rand_bytes(n)); }

int64_t now_unix() { return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }
int64_t now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch()).count(); }

std::string rfc3339_now() {
    std::time_t t = std::time(nullptr);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%SZ", &tm);
    return buf;
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return char(std::tolower(c)); });
    return s;
}
bool iequals(std::string_view a, std::string_view b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return std::tolower((unsigned char)x) == std::tolower((unsigned char)y); });
}
std::string trim(std::string_view s) {
    size_t a = 0, b = s.size();
    while (a < b && std::isspace((unsigned char)s[a])) a++;
    while (b > a && std::isspace((unsigned char)s[b - 1])) b--;
    return std::string(s.substr(a, b - a));
}
std::vector<std::string> split_csv(std::string_view s) {
    std::vector<std::string> out;
    size_t i = 0;
    while (i <= s.size()) {
        size_t j = s.find(',', i);
        if (j == std::string_view::npos) j = s.size();
        auto t = trim(s.substr(i, j - i));
        if (!t.empty()) out.push_back(t);
        i = j + 1;
    }
    return out;
}
std::string env_or(const char* name, const std::string& def) {
    const char* v = std::getenv(name);
    return (v && *v) ? std::string(v) : def;
}
std::vector<std::string> env_list(const char* name) { return split_csv(env_or(name)); }
bool contains_ci(const std::vector<std::string>& list, std::string_view v) {
    for (auto& l : list)
        if (iequals(l, v)) return true;
    return false;
}
std::string domain_of(std::string_view email) {
    auto i = email.rfind('@');
    return i == std::string_view::npos ? std::string() : lower(std::string(email.substr(i + 1)));
}

std::string url_encode(std::string_view s) {
    std::string o;
    for (unsigned char c : s) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') o += char(c);
        else { char b[4]; std::snprintf(b, sizeof b, "%%%02X", c); o += b; }
    }
    return o;
}
std::string url_decode(std::string_view v) {
    std::string out;
    for (size_t p = 0; p < v.size(); p++) {
        if (v[p] == '%' && p + 2 < v.size() && std::isxdigit((unsigned char)v[p + 1]) && std::isxdigit((unsigned char)v[p + 2])) {
            out += char(std::stoi(std::string(v.substr(p + 1, 2)), nullptr, 16));
            p += 2;
        } else if (v[p] == '+') out += ' ';
        else out += v[p];
    }
    return out;
}
std::string query_get(std::string_view query, std::string_view name) {
    size_t i = 0;
    while (i < query.size()) {
        size_t j = query.find('&', i);
        if (j == std::string_view::npos) j = query.size();
        auto kv = query.substr(i, j - i);
        auto eq = kv.find('=');
        if (kv.substr(0, eq) == name) return eq == std::string_view::npos ? std::string() : url_decode(kv.substr(eq + 1));
        i = j + 1;
    }
    return {};
}

Url parse_url(const std::string& url) {
    Url u;
    auto p = url.find("://");
    if (p == std::string::npos) throw Error("bad url: " + url);
    u.scheme = lower(url.substr(0, p));
    auto rest = url.substr(p + 3);
    auto slash = rest.find_first_of("/?#");
    std::string hostport = rest.substr(0, slash);
    // Strict authority: no userinfo ("claude.ai:443@evil.example" is evil.example to a browser), no backslash, no
    // whitespace or control characters, no percent-encoding; IPv6 only in brackets.
    for (unsigned char c : hostport)
        if (!(std::isalnum(c) || c == '.' || c == '-' || c == ':' || c == '[' || c == ']' || c == '_')) throw Error("bad url (authority): " + url);
    for (unsigned char c : url)
        if (c <= 0x20 || c == 0x7f || c == '\\') throw Error("bad url (character): " + url);
    u.target = slash == std::string::npos ? "/" : rest.substr(slash);
    if (u.target[0] == '?') u.target = "/" + u.target;
    std::string port;
    if (!hostport.empty() && hostport[0] == '[') {  // [IPv6]:port
        auto close = hostport.find(']');
        if (close == std::string::npos) throw Error("bad url (host): " + url);
        u.host = hostport.substr(0, close + 1);
        std::string after = hostport.substr(close + 1);
        if (!after.empty()) {
            if (after[0] != ':') throw Error("bad url (host): " + url);
            port = after.substr(1);
            if (port.empty()) throw Error("bad url (port): " + url);
        }
    } else {
        if (hostport.find_first_of("[]") != std::string::npos) throw Error("bad url (host): " + url);
        auto colon = hostport.find(':');
        u.host = hostport.substr(0, colon);
        if (colon != std::string::npos) {
            port = hostport.substr(colon + 1);
            if (port.empty()) throw Error("bad url (port): " + url);
        }
    }
    if (!port.empty()) {
        bool digits = port.size() <= 5 && std::all_of(port.begin(), port.end(), [](unsigned char c) { return std::isdigit(c); });
        if (!digits || std::stoi(port) < 1 || std::stoi(port) > 65535) throw Error("bad url (port): " + url);
        u.port = port;
    }
    if (u.port.empty()) u.port = u.tls() ? "443" : "80";
    if (u.host.empty()) throw Error("bad url (no host): " + url);
    return u;
}

}  // namespace ffy
