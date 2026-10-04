#pragma once
// Shared contract of the remote MCP work (phases 1-4). ADDITIVE changes only, through the orchestrator.
// Phase 1 (protocol) consumes Principal/EffectivePolicy; phase 2 (auth) produces them; nobody else defines them.

#include <functional>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace fairyfly::mcp {

/// Who is calling. Stdio: one implicit local principal (name "stdio", unrestricted, expires never).
struct Principal {
    std::string name = "stdio";          ///< token name, never the secret
    std::string id;                      ///< token id (unique per created token, never the secret); empty for stdio/insecure
    std::set<std::string> scopes;        ///< tool families (session, screen, ...); empty + all_scopes=false => none
    bool all_scopes = true;              ///< "*": every family
    std::vector<std::string> sap_systems;///< allowed "SID/CLIENT" pairs ("A4H/001"); empty = any
    std::vector<std::string> sap_identities;///< exact "SID/CLIENT/USER" grants; empty = legacy/unbound
    std::vector<std::string> tcodes;     ///< allowed T-codes (glob, case-insensitive); empty = any
    std::vector<std::string> connections;///< allowed saved connection / SAP Logon entry names (glob, case-insensitive); empty = any
    int rate_per_minute = 0;             ///< 0 = server default
    std::map<std::string, int> rate_families; ///< extra calls-per-minute budget per tool family (e.g. element=10); empty = none
    bool read_only = false;              ///< token can only narrow the server mode
    bool allow_navigation = false;       ///< with a T-code allowlist: menus and navigating keys stay usable (--allow-navigation)
    bool allow_selection_input = false;  ///< read-only token with a T-code allowlist: may fill selection fields on the initial screen (--allow-selection-input)
    std::string remote_addr;             ///< client address: the socket peer address (forwarded headers are never trusted)
    bool authenticated = false;          ///< false for stdio and for --insecure-no-auth
};

/// Key of the per-principal in-memory state (blocked flag, sticky connection, rate budgets): the token id when known,
/// so a deleted and recreated token with the same name starts clean; the name otherwise (stdio, insecure).
inline const std::string& principal_key(const Principal& p) { return p.id.empty() ? p.name : p.id; }

/// Protocol era of one request (HTTP) or connection (stdio).
enum class ProtocolEra { Legacy, Stateless };

/// Transport label written to the audit trail.
inline const char* transport_label(bool http) { return http ? "http" : "stdio"; }


/// Raw request facts the authenticator needs (HTTP adapter fills this; header names are case-insensitive upstream).
struct AuthRequest {
    std::string authorization;      ///< "Authorization" header value ("" if absent)
    std::string peer_addr;          ///< socket peer address (the client address used for IP binding)
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
    /// Force the next authentication to read current token metadata before a queued job executes.
    virtual void invalidate_cache() {}
    /// Human-readable startup warnings (no tokens configured, insecure mode, ...). For the posture banner/tray.
    virtual std::vector<std::string> posture_warnings() const { return {}; }
};

/// Snapshot shown by the tray/status.
struct ServerStatus {
    bool running = false;
    std::string endpoint;               ///< "http://127.0.0.1:8383/mcp" or "stdio"
    bool read_only = true;
    bool parallel_sessions = false;     ///< owner-routed HTTP calls use one ordered lane per SAP window
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
