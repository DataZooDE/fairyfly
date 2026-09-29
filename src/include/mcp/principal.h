#pragma once
// Shared contract of the remote MCP work (phases 1-4). ADDITIVE changes only, through the orchestrator.
// Phase 1 (protocol) consumes Principal/EffectivePolicy; phase 2 (auth) produces them; nobody else defines them.

#include <functional>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace fairyfly::mcp {

/// Who is calling. Stdio: one implicit local principal (name "stdio", unrestricted, expires never).
struct Principal {
    std::string name = "stdio";          ///< token name, never the secret
    std::set<std::string> scopes;        ///< tool families (session, screen, ...); empty + all_scopes=false => none
    bool all_scopes = true;              ///< "*": every family
    std::vector<std::string> sap_systems;///< allowed "SID/CLIENT" pairs ("A4H/001"); empty = any
    std::vector<std::string> tcodes;     ///< allowed T-codes (glob, case-insensitive); empty = any
    std::vector<std::string> connections;///< allowed saved connection / SAP Logon entry names (glob, case-insensitive); empty = any
    int rate_per_minute = 0;             ///< 0 = server default
    bool read_only = false;              ///< token can only narrow the server mode
    std::string remote_addr;             ///< client address (from proxy header only when the proxy secret matched)
    bool authenticated = false;          ///< false for stdio and for --insecure-no-auth
};

/// Protocol era of one request (HTTP) or connection (stdio).
enum class ProtocolEra { Legacy, Stateless };

/// Transport label written to the audit trail.
inline const char* transport_label(bool http) { return http ? "http" : "stdio"; }


/// Raw request facts the authenticator needs (HTTP adapter fills this; header names are case-insensitive upstream).
struct AuthRequest {
    std::string authorization;      ///< "Authorization" header value ("" if absent)
    std::string proxy_secret;       ///< "X-Fairyfly-Proxy-Secret" value
    std::string forwarded_for;      ///< "X-Forwarded-For" value
    std::string forwarded_proto;    ///< "X-Forwarded-Proto" value
    std::string peer_addr;          ///< socket peer address (127.0.0.1 when IIS fronts us)
};

/// Result of authenticating one request.
struct AuthOutcome {
    bool ok = false;
    int http_status = 401;          ///< 401 / 403 / 429 when !ok
    std::string error_code;         ///< machine code: AUTH_REQUIRED, TOKEN_INVALID, TOKEN_EXPIRED, IP_NOT_ALLOWED, ...
    std::string message;            ///< safe, model/human-readable; never contains secrets
    std::string www_authenticate;   ///< value for the WWW-Authenticate header on 401
    Principal principal;            ///< valid when ok
};

/// Implemented by phase 2 (token store), consumed by phase 1 (HTTP endpoint). Thread-safe.
class IAuthenticator {
public:
    virtual ~IAuthenticator() = default;
    virtual AuthOutcome authenticate(const AuthRequest& request) = 0;
    /// Human-readable startup warnings (no tokens configured, insecure mode, ...). For the posture banner/tray.
    virtual std::vector<std::string> posture_warnings() const { return {}; }
};

/// Snapshot shown by the tray/status.
struct ServerStatus {
    bool running = false;
    std::string endpoint;               ///< "http://127.0.0.1:8383/mcp" or "stdio"
    bool read_only = true;
    std::vector<std::string> warnings;
    long long calls_total = 0;
    long long calls_denied = 0;
};

/// Implemented by phase 1 (the running server), consumed by phase 4 (tray). Thread-safe.
class IServerControl {
public:
    virtual ~IServerControl() = default;
    virtual ServerStatus status() const = 0;
    /// Effective server mode: true = read-only guard. Turning write mode ON is the tray's job to confirm.
    virtual void set_read_only(bool read_only) = 0;
    virtual void request_stop() = 0;      ///< graceful: finish the running call, stop accepting
    virtual void request_restart() = 0;   ///< stop, re-read config, start again in the same process
    /// Start again after a user Stop (tray "Start"). Additive in phase 4; default = request_restart(), which must
    /// also work while stopped.
    virtual void request_start() { request_restart(); }
};

} // namespace fairyfly::mcp
