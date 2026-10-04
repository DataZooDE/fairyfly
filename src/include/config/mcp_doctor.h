#pragma once
// `fairyfly mcp doctor`: pure checks over injectable probes. The real probes (TCP, SAP GUI via the
// existing read-only `doctor` logic, desktop/session state, registry, mutex) live in
// src/commands/mcp_doctor_command.cpp; tests use fakes.

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

#include "include/auth/token_store.h"
#include "include/config/mcp_config.h"

namespace fairyfly::config {

enum class CheckStatus { Pass, Warn, Fail, Skip };
const char* check_status_name(CheckStatus status);

struct DoctorCheck {
    std::string id;
    CheckStatus status = CheckStatus::Skip;
    std::string message;
    std::string remediation;
};

enum class PortState { Free, InUse, Unknown };

struct SapState {
    enum class Tri { Yes, No, Unknown };
    Tri scripting_available = Tri::Unknown;   ///< COM scripting engine reachable
    Tri session_present = Tri::Unknown;       ///< a logged-in session exists
    std::string detail;
};

// A visible SAP GUI window can still be the unauthenticated logon screen.
// nullopt means its login facts could not be read reliably.
SapState::Tri classify_logged_in_users(const std::vector<std::optional<std::string>>& users,
                                       bool enumeration_complete) noexcept;
bool owner_sap_identity_allowed(const std::string& system, const std::string& client,
                                const std::string& user, const std::vector<std::string>& identities);

struct DesktopState {
    std::optional<unsigned> session_id;       ///< ProcessIdToSessionId
    bool interactive_window_station = true;   ///< WinSta0
};

enum class LockState { Active, Locked, RdpDisconnected, Unknown };

/// What `mcp doctor` asks about the http.sys setup (see src/setup/setup_doctor.cpp for the real collector).
struct SetupQuery {
    bool tls = false;
    std::string hostname;      ///< name clients use (certificate SAN)
    int port = 8383;
};

/// Facts about the http.sys setup; every "known" flag is false when the value could not be read.
struct SetupFacts {
    bool available = false;                 ///< false: nothing was collected (all setup checks skip)
    std::string elevation;                  ///< elevated | admin_filtered | standard_user | unknown
    bool manifest_present = false;
    bool manifest_unreadable = false;
    std::string manifest_path;
    std::string manifest_mode;              ///< tls | no-tls
    std::string manifest_hostname;
    int manifest_port = 0;
    std::string manifest_thumbprint;
    std::string manifest_firewall_rule;
    std::string prefix;                     ///< the effective URL prefix
    bool urlacl_known = false;
    bool urlacl_reserved = false;
    bool urlacl_covers_user = false;
    bool ssl_known = false;
    bool ssl_v4 = false;
    bool ssl_v6 = false;
    bool ssl_foreign = false;               ///< a binding with another AppId
    std::string ssl_thumbprint;             ///< of the 0.0.0.0 binding
    bool cert_known = false;
    bool cert_found = false;
    bool cert_has_key = false;
    bool cert_san_ok = false;
    long long cert_not_after = 0;
    long long now = 0;
    std::string cert_thumbprint;
    bool firewall_known = false;
    bool firewall_exists = false;
    bool port_listening = false;
    std::string tls_status;                 ///< ok | unreachable | handshake_failed | error (empty = not probed)
    std::string tls_protocol;
    bool tls_thumbprint_match = false;
};

class DoctorProbes {
public:
    virtual ~DoctorProbes() = default;
    virtual PortState probe_port(const std::string& host, int port) = 0;
    virtual SapState sap() = 0;                       ///< read-only: never clicks or fills
    virtual DesktopState desktop() = 0;
    virtual LockState lock_state() = 0;
    virtual std::optional<int> token_count() = 0;     ///< nullopt = unknown (phase 2 fills this in)
    virtual std::optional<std::string> autostart_command() = 0;   ///< value of HKCU Run "fairyfly-mcp"
    virtual bool tray_running() = 0;                  ///< named-mutex probe
    /// http.sys setup facts (read-only, never elevated). Default: nothing collected.
    virtual SetupFacts setup_facts(const SetupQuery& query) { (void)query; return {}; }
    /// True when the retired proxy-secret credential `fairyfly:fairyfly-mcp-proxy` still exists.
    virtual bool legacy_proxy_secret() { return false; }
};

/// Result of loading the config file (done by the caller, so the checks stay pure).
struct ConfigState {
    std::string path;
    bool exists = false;
    bool read_error = false;
    std::vector<ConfigIssue> issues;   ///< parse issues (errors and warnings)
};

struct DoctorInput {
    ConfigState config;
    std::string transport = "stdio";   ///< effective server.transport
    std::string host = "127.0.0.1";
    int port = 8383;
    std::string exe_path;              ///< current fairyfly.exe (autostart comparison)
    bool tls = false;                  ///< effective server.tls (https listener)
    std::string hostname;              ///< host name clients use; empty = unknown
};

std::vector<DoctorCheck> run_mcp_doctor(const DoctorInput& input, DoctorProbes& probes);

/// Count of tokens that are neither revoked nor expired (read-only). nullopt when the store cannot be read.
std::optional<int> count_active_tokens(auth::TokenStore& store);

/// "fail" when any check failed, else "warn" when any warned, else "pass".
std::string overall_status(const std::vector<DoctorCheck>& checks);
std::string format_doctor_text(const std::vector<DoctorCheck>& checks);
nlohmann::json doctor_to_json(const std::vector<DoctorCheck>& checks);

} // namespace fairyfly::config
