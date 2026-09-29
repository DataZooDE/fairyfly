#pragma once
// Pure web.config generator for the IIS reverse proxy in front of `fairyfly mcp --http`.
// No I/O; the caller validates inputs first (validators.h) and this function re-checks the invariants
// it relies on (it throws std::invalid_argument for anything it would have to escape unsafely).

#include <cstdint>
#include <string>
#include <vector>

namespace fairyfly::iis {

/// The one and only public path; everything else is answered with 404 by IIS.
inline constexpr const char* kMcpPath = "/mcp";
/// Header carrying the shared secret to fairyfly (server variable HTTP_X_FAIRYFLY_PROXY_SECRET).
inline constexpr const char* kProxySecretHeader = "X-Fairyfly-Proxy-Secret";
/// Placeholder used wherever the secret must not be printed.
inline constexpr const char* kSecretPlaceholder = "@@FAIRYFLY_PROXY_SECRET@@";

struct WebConfigParams {
    std::string upstream = "http://127.0.0.1:8383";  ///< loopback base URL of the fairyfly HTTP server
    std::string proxy_secret;                         ///< injected as X-Fairyfly-Proxy-Secret
    std::vector<std::string> allow_ips;               ///< IPv4/IPv6 addresses or CIDR ranges
    bool allow_any_ip = false;                        ///< no ipSecurity restriction (loud warning elsewhere)
    int proxy_timeout_seconds = 300;                  ///< must exceed the MCP call timeout (120 s default)
    int max_content_bytes = 1048576;                  ///< request filtering: 1 MiB
    int hsts_max_age_seconds = 31536000;
};

/// Deterministic UTF-8 XML text.
std::string generate_web_config(const WebConfigParams& params);

/// "00:05:00" style time span for the ARR proxy timeout.
std::string format_timespan(int seconds);

/// Server variables the rewrite rules set (must be listed in allowedServerVariables by the setup).
const std::vector<std::string>& required_server_variables();

/// 64-bit FNV-1a, lower-case hex. Non-cryptographic: drift detection only.
std::string content_hash(const std::string& text);

/// Replaces every occurrence of `secret` in `text` with kSecretPlaceholder (no-op for an empty secret).
std::string mask_secret(std::string text, const std::string& secret);

} // namespace fairyfly::iis
