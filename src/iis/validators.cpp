#include "include/iis/validators.h"

#include <algorithm>
#include <cctype>

namespace fairyfly::iis {
namespace {

bool is_alnum(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
bool is_digit(char c) { return c >= '0' && c <= '9'; }
bool is_hex(char c) { return is_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) { out.push_back(cur); cur.clear(); }
        else cur += c;
    }
    out.push_back(cur);
    return out;
}

bool parse_uint(const std::string& s, int max_value, int& out) {
    if (s.empty() || s.size() > 6) return false;
    int v = 0;
    for (char c : s) {
        if (!is_digit(c)) return false;
        v = v * 10 + (c - '0');
    }
    if (v > max_value) return false;
    out = v;
    return true;
}

bool is_ipv4(const std::string& s) {
    const auto parts = split(s, '.');
    if (parts.size() != 4) return false;
    for (const auto& p : parts) {
        int v = 0;
        if (p.size() > 3 || !parse_uint(p, 255, v)) return false;
    }
    return true;
}

bool is_ipv6(const std::string& s) {
    if (s.size() < 2 || s.size() > 39) return false;
    int colons = 0;
    bool double_colon = false;
    size_t group = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == ':') {
            ++colons;
            if (i > 0 && s[i - 1] == ':') {
                if (double_colon) return false;
                double_colon = true;
            }
            group = 0;
        } else if (is_hex(c)) {
            if (++group > 4) return false;
        } else {
            return false;
        }
    }
    if (colons < 2 || colons > 7) return false;
    if (!double_colon && colons != 7) return false;
    return s.find(":::") == std::string::npos;
}

} // namespace

std::optional<std::string> hostname_error(const std::string& value) {
    if (value.empty()) return "hostname is empty";
    if (value.size() > 253) return "hostname is longer than 253 characters";
    if (is_ipv4(value)) return "hostname must be a DNS name, not an IP address";
    for (const auto& label : split(value, '.')) {
        if (label.empty() || label.size() > 63) return "hostname has an empty or over-long label";
        if (!is_alnum(label.front()) || !is_alnum(label.back())) return "hostname labels must start and end with a letter or digit";
        for (char c : label) {
            if (!is_alnum(c) && c != '-') return "hostname may only contain letters, digits, '-' and '.'";
        }
    }
    return std::nullopt;
}

std::optional<std::string> thumbprint_error(const std::string& value) {
    if (value.size() != 40) return "thumbprint must be exactly 40 hex digits";
    if (!std::all_of(value.begin(), value.end(), is_hex)) return "thumbprint may only contain hex digits";
    return std::nullopt;
}

std::string normalize_thumbprint(const std::string& value) {
    std::string out = value;
    for (auto& c : out) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return out;
}

std::optional<std::string> cidr_error(const std::string& value) {
    if (value.empty()) return "address is empty";
    const auto slash = value.find('/');
    const std::string addr = value.substr(0, slash);
    int prefix = -1;
    if (slash != std::string::npos) {
        int p = 0;
        if (!parse_uint(value.substr(slash + 1), 128, p)) return "prefix length is not a number";
        prefix = p;
    }
    if (is_ipv4(addr)) {
        if (prefix > 32) return "IPv4 prefix length must be 0-32";
        return std::nullopt;
    }
    if (is_ipv6(addr)) return std::nullopt;
    return "not a valid IPv4/IPv6 address or CIDR range";
}

std::optional<std::string> port_error(int value) {
    if (value < 1 || value > 65535) return "port must be between 1 and 65535";
    return std::nullopt;
}

std::optional<std::string> site_name_error(const std::string& value) {
    if (value.empty() || value.size() > 63) return "site name must be 1-63 characters";
    if (!is_alnum(value.front())) return "site name must start with a letter or digit";
    for (char c : value) {
        if (!is_alnum(c) && c != '.' && c != '_' && c != '-') return "site name may only contain letters, digits, '.', '_' and '-'";
    }
    return std::nullopt;
}

std::optional<std::string> site_path_error(const std::string& value) {
    if (value.size() < 4 || value.size() > 200) return "site path must be an absolute path (4-200 characters)";
    if (!is_alnum(value[0]) || is_digit(value[0]) || value[1] != ':' || value[2] != '\\')
        return "site path must look like C:\\inetpub\\name";
    for (size_t i = 3; i < value.size(); ++i) {
        const char c = value[i];
        if (!is_alnum(c) && c != ' ' && c != '_' && c != '.' && c != '-' && c != '\\') return "site path contains an unsafe character";
    }
    if (value.back() == '\\' || value.back() == ' ' || value.back() == '.') return "site path must not end with '\\', ' ' or '.'";
    for (const auto& seg : split(value.substr(3), '\\')) {
        if (seg.empty()) return "site path contains an empty segment";
        if (seg == "." || seg == "..") return "site path must not contain '.' or '..' segments";
    }
    return std::nullopt;
}

std::optional<UpstreamParts> parse_upstream(const std::string& value) {
    const std::string prefix = "http://";
    if (value.compare(0, prefix.size(), prefix) != 0) return std::nullopt;
    const std::string rest = value.substr(prefix.size());
    const auto colon = rest.rfind(':');
    if (colon == std::string::npos) return std::nullopt;
    UpstreamParts parts;
    parts.host = rest.substr(0, colon);
    if (parts.host != "127.0.0.1" && parts.host != "localhost" && parts.host != "[::1]") return std::nullopt;
    if (!parse_uint(rest.substr(colon + 1), 65535, parts.port) || parts.port < 1) return std::nullopt;
    return parts;
}

std::optional<std::string> upstream_error(const std::string& value) {
    if (!parse_upstream(value))
        return "upstream must be http://127.0.0.1:PORT, http://localhost:PORT or http://[::1]:PORT (loopback only, no path)";
    return std::nullopt;
}

std::optional<Ipv4Range> ipv4_range(const std::string& cidr) {
    if (cidr_error(cidr)) return std::nullopt;
    const auto slash = cidr.find('/');
    const std::string addr = cidr.substr(0, slash);
    if (!is_ipv4(addr)) return std::nullopt;
    int prefix = 32;
    if (slash != std::string::npos) parse_uint(cidr.substr(slash + 1), 32, prefix);
    const unsigned long mask = prefix == 0 ? 0UL : (0xFFFFFFFFUL << (32 - prefix)) & 0xFFFFFFFFUL;
    unsigned long value = 0;
    for (const auto& o : split(addr, '.')) value = (value << 8) | static_cast<unsigned long>(std::stoi(o));
    value &= mask;
    auto dotted = [](unsigned long v) {
        return std::to_string((v >> 24) & 255) + "." + std::to_string((v >> 16) & 255) + "." +
               std::to_string((v >> 8) & 255) + "." + std::to_string(v & 255);
    };
    return Ipv4Range{dotted(value), dotted(mask)};
}

} // namespace fairyfly::iis
