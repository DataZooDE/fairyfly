#include "include/auth/ip.h"

#include <winsock2.h>
#include <ws2tcpip.h>

#include <algorithm>
#include <cctype>
#include <cstring>

#pragma comment(lib, "ws2_32.lib")

namespace fairyfly::auth {

namespace {

std::string trim(const std::string& s) {
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const auto last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::optional<IpBytes> parse_plain(const std::string& text) {
    IpBytes out{};
    in_addr v4{};
    if (inet_pton(AF_INET, text.c_str(), &v4) == 1) {
        out[10] = 0xff;
        out[11] = 0xff;
        std::memcpy(out.data() + 12, &v4, 4);
        return out;
    }
    in6_addr v6{};
    if (inet_pton(AF_INET6, text.c_str(), &v6) == 1) {
        std::memcpy(out.data(), &v6, 16);
        return out;
    }
    return std::nullopt;
}

} // namespace

std::optional<IpBytes> parse_ip(const std::string& raw) {
    std::string text = trim(raw);
    if (text.empty()) return std::nullopt;
    if (text.front() == '[') {
        const auto close = text.find(']');
        if (close == std::string::npos) return std::nullopt;
        return parse_plain(text.substr(1, close - 1));
    }
    if (auto direct = parse_plain(text)) return direct;
    // "1.2.3.4:5678" (a single colon is never an IPv6 literal)
    if (std::count(text.begin(), text.end(), ':') == 1) return parse_plain(text.substr(0, text.find(':')));
    // zone id "fe80::1%eth0"
    if (const auto pct = text.find('%'); pct != std::string::npos) return parse_plain(text.substr(0, pct));
    return std::nullopt;
}

std::optional<IpRule> parse_ip_rule(const std::string& raw) {
    const std::string text = trim(raw);
    if (text.empty()) return std::nullopt;
    const auto slash = text.find('/');
    IpRule rule;
    const std::string addr = slash == std::string::npos ? text : text.substr(0, slash);
    const auto parsed = parse_plain(addr);
    if (!parsed) return std::nullopt;
    const bool is_v4 = addr.find(':') == std::string::npos;
    rule.network = *parsed;
    int bits = is_v4 ? 32 : 128;
    if (slash != std::string::npos) {
        const std::string len = text.substr(slash + 1);
        if (len.empty() || len.size() > 3 || !std::all_of(len.begin(), len.end(), [](unsigned char c) { return std::isdigit(c) != 0; }))
            return std::nullopt;
        bits = std::stoi(len);
        if (bits < 0 || bits > (is_v4 ? 32 : 128)) return std::nullopt;
    }
    rule.prefix_bits = is_v4 ? bits + 96 : bits;
    return rule;
}

bool ip_in_rule(const IpBytes& address, const IpRule& rule) {
    int bits = rule.prefix_bits;
    for (std::size_t i = 0; i < 16 && bits > 0; ++i, bits -= 8) {
        const unsigned char mask = bits >= 8 ? 0xff : static_cast<unsigned char>(0xff << (8 - bits));
        if ((address[i] & mask) != (rule.network[i] & mask)) return false;
    }
    return true;
}

bool ip_allowed(const std::string& address, const std::vector<std::string>& rules) {
    const auto parsed = parse_ip(address);
    if (!parsed) return false;
    for (const auto& text : rules) {
        const auto rule = parse_ip_rule(text);
        if (rule && ip_in_rule(*parsed, *rule)) return true;
    }
    return false;
}

} // namespace fairyfly::auth
