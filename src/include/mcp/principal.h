#pragma once
// Shared contract of the remote MCP work (phases 1-4). ADDITIVE changes only, through the orchestrator.
// Phase 1 (protocol) consumes Principal/EffectivePolicy; phase 2 (auth) produces them; nobody else defines them.

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
    int rate_per_minute = 0;             ///< 0 = server default
    bool read_only = false;              ///< token can only narrow the server mode
    std::string remote_addr;             ///< client address (from proxy header only when the proxy secret matched)
    bool authenticated = false;          ///< false for stdio and for --insecure-no-auth
};

/// Protocol era of one request (HTTP) or connection (stdio).
enum class ProtocolEra { Legacy, Stateless };

/// Transport label written to the audit trail.
inline const char* transport_label(bool http) { return http ? "http" : "stdio"; }

} // namespace fairyfly::mcp
