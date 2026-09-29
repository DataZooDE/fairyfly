#pragma once
// IP address / CIDR helpers for token IP binding (IPv4 and IPv6; IPv4-mapped IPv6 is normalised to IPv4).

#include <array>
#include <optional>
#include <string>
#include <vector>

namespace fairyfly::auth {

/// 16-byte form; IPv4 addresses are stored as ::ffff:a.b.c.d.
using IpBytes = std::array<unsigned char, 16>;

/// Parses "1.2.3.4", "::1", "[::1]", "1.2.3.4:5678", "[::1]:80"; nullopt when not an address.
std::optional<IpBytes> parse_ip(const std::string& text);

struct IpRule {
    IpBytes network{};
    int prefix_bits = 128;  ///< always in 16-byte space (IPv4 /24 -> 120)
};
/// "10.0.0.0/8", "192.168.1.5", "fd00::/8". nullopt when invalid.
std::optional<IpRule> parse_ip_rule(const std::string& text);

bool ip_in_rule(const IpBytes& address, const IpRule& rule);
/// True when `address` matches any rule; false for an unparsable address or rule (fail closed).
bool ip_allowed(const std::string& address, const std::vector<std::string>& rules);

} // namespace fairyfly::auth
