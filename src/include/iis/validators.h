#pragma once
// Strict input validators for the IIS integration. Every value that reaches PowerShell or web.config
// passes through one of these first; nothing is ever concatenated into a script.

#include <optional>
#include <string>
#include <vector>

namespace fairyfly::iis {

/// Each *_error returns nullopt when the value is valid, otherwise a safe message.

/// RFC 1123 host name (letters, digits, hyphen, dots), no wildcard, not an IP literal.
std::optional<std::string> hostname_error(const std::string& value);
/// 40 hex digits (SHA-1 thumbprint). Spaces/colons are NOT accepted.
std::optional<std::string> thumbprint_error(const std::string& value);
std::string normalize_thumbprint(const std::string& value);  ///< upper-case
/// IPv4 "a.b.c.d[/n]" or IPv6 "x:x::x[/n]".
std::optional<std::string> cidr_error(const std::string& value);
std::optional<std::string> port_error(int value);
std::optional<std::string> site_name_error(const std::string& value);
/// Absolute Windows path "C:\dir\sub" made of safe characters, no "..".
std::optional<std::string> site_path_error(const std::string& value);
/// "http://127.0.0.1:8383" / localhost / [::1]; loopback only, no path.
std::optional<std::string> upstream_error(const std::string& value);

struct UpstreamParts {
    std::string host;  ///< as written ("127.0.0.1", "localhost", "[::1]")
    int port = 0;
};
std::optional<UpstreamParts> parse_upstream(const std::string& value);

/// IPv4 CIDR as network address plus dotted subnet mask ("10.0.0.5/8" -> "10.0.0.0","255.0.0.0"). nullopt for IPv6.
struct Ipv4Range {
    std::string address;
    std::string mask;
};
std::optional<Ipv4Range> ipv4_range(const std::string& cidr);

} // namespace fairyfly::iis
