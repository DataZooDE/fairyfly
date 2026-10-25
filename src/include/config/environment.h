#pragma once
// The one reader of fairyfly's environment variables. Every variable fairyfly reads is named in `env` below, every
// switch is parsed by parse_flag, and code reads the process environment through Environment::process() (or takes an
// EnvLookup so tests can inject one). No other file calls getenv/_dupenv_s.
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace fairyfly::config {

/// Looks up one variable; "" when unset.
using EnvLookup = std::function<std::string(const char*)>;

namespace env {
inline constexpr const char* kReadOnly = "FAIRYFLY_READ_ONLY";          // hard read-only cap (a flag)
inline constexpr const char* kAudit = "FAIRYFLY_AUDIT";                 // 0|off|required
inline constexpr const char* kAuditFile = "FAIRYFLY_AUDIT_FILE";
inline constexpr const char* kLogLevel = "FFLYLOG_LEVEL";
inline constexpr const char* kCacheDir = "FAIRYFLY_CACHE_DIR";
inline constexpr const char* kDiag = "FAIRYFLY_DIAG";                   // live diagnostics (a flag)
inline constexpr const char* kBulkFault = "FAIRYFLY_BULK_FAULT";        // with kDiag: simulated GetObjectTree faults
inline constexpr const char* kProbeAll = "FAIRYFLY_PROBE_ALL";          // a flag
inline constexpr const char* kScreenReader = "FAIRYFLY_SCREEN_READER";  // legacy|bulk|auto
inline constexpr const char* kGithubToken = "FAIRYFLY_GITHUB_TOKEN";
inline constexpr const char* kUpdateCheck = "FAIRYFLY_UPDATE_CHECK";
inline constexpr const char* kMcpConfig = "FAIRYFLY_MCP_CONFIG";
inline constexpr const char* kMcpPrefix = "FAIRYFLY_MCP_";             // FAIRYFLY_MCP_<KEY> config layer
inline constexpr const char* kTrayChild = "FAIRYFLY_TRAY_CHILD";
inline constexpr const char* kTrayLaunched = "FAIRYFLY_TRAY_LAUNCHED";
inline constexpr const char* kPsParams = "FAIRYFLY_PS_PARAMS";          // parameters handed to a PowerShell script
inline constexpr const char* kToken = "FAIRYFLY_TOKEN";                 // client side: the bearer token (client-config)
inline constexpr const char* kAuthHeader = "FAIRYFLY_AUTH_HEADER";      // client side: "Bearer <token>" (client-config)
inline constexpr const char* kLinkGateway = "FAIRYFLY_LINK_GATEWAY";
inline constexpr const char* kLinkForbidDefaultState = "FAIRYFLY_LINK_FORBID_DEFAULT_STATE";
inline constexpr const char* kLocalAppData = "LOCALAPPDATA";
inline constexpr const char* kHome = "HOME";
}  // namespace env

/// Trimmed and lower-cased (ASCII): how every switch value is compared.
std::string normalized(std::string_view value);
/// A switch: on for 1/true/yes/on in any case, ignoring surrounding white space; off for anything else.
bool parse_flag(std::string_view value);
/// Explicitly off: 0/false/no/off under the same rules (for switches that are on by default, e.g. FAIRYFLY_AUDIT).
bool parse_off(std::string_view value);

class Environment {
public:
    explicit Environment(EnvLookup lookup);

    /// The real process environment, read live on every call (one shared instance).
    static const Environment& process();

    std::string get(const char* name) const;
    bool flag(const char* name) const { return parse_flag(get(name)); }
    /// A switch that is on by default: off only for parse_off (FAIRYFLY_UPDATE_CHECK).
    bool enabled(const char* name) const { return !parse_off(get(name)); }
    /// FAIRYFLY_READ_ONLY: refuse every state-changing action, whatever flags or tokens say.
    bool read_only_cap() const { return flag(env::kReadOnly); }
    /// LOCALAPPDATA, or the temp directory when it is unset (an empty path when that cannot be resolved; never throws).
    std::filesystem::path local_app_data() const;
    /// local_app_data() / "fairyfly": where fairyfly keeps its config, logs, audit trail and state.
    std::filesystem::path data_dir() const { return local_app_data() / "fairyfly"; }
    const EnvLookup& lookup() const { return lookup_; }

private:
    EnvLookup lookup_;
};

/// Environment::process().lookup(): for APIs that take an EnvLookup.
EnvLookup process_env();
/// Environment::process().read_only_cap().
inline bool read_only_cap() { return Environment::process().read_only_cap(); }

}  // namespace fairyfly::config
