#pragma once
// Values of the MCP server that several parts need: one definition each.
#include <cstddef>
#include <string>
#include <vector>

namespace fairyfly::mcp {

inline constexpr int kDefaultHttpPort = 8383;  ///< plain HTTP (loopback, development) and `--no-tls` setups
inline constexpr int kDefaultTlsPort = 8443;   ///< HTTPS after `mcp setup`

/// WWW-Authenticate of a 401 (RFC 6750); with error="invalid_token" when a token was given but is not valid.
inline constexpr const char* kBearerChallenge = "Bearer realm=\"fairyfly\"";
inline constexpr const char* kBearerChallengeInvalidToken = "Bearer realm=\"fairyfly\", error=\"invalid_token\"";

/// The stateless MCP protocol era (HTTP only).
inline constexpr const char* kStatelessVersion = "2026-07-28";

/// Every protocol version fairyfly speaks, newest first, and where.
struct ProtocolVersion {
    const char* version;
    bool stdio;
    bool http;
};
inline constexpr ProtocolVersion kProtocolVersions[] = {
    {kStatelessVersion, false, true},
    {"2025-11-25", true, true},
    {"2025-06-18", true, true},
    {"2025-03-26", true, false},
    {"2024-11-05", true, false},
};

/// stdio (initialize/session era), newest first.
inline std::vector<std::string> stdio_protocol_versions() {
    std::vector<std::string> out;
    for (const auto& v : kProtocolVersions)
        if (v.stdio) out.emplace_back(v.version);
    return out;
}

/// HTTP (stateless era first, then the legacy versions it still accepts), newest first.
inline std::vector<std::string> http_protocol_versions() {
    std::vector<std::string> out;
    for (const auto& v : kProtocolVersions)
        if (v.http) out.emplace_back(v.version);
    return out;
}

}  // namespace fairyfly::mcp
