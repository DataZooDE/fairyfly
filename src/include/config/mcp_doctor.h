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

struct DesktopState {
    std::optional<unsigned> session_id;       ///< ProcessIdToSessionId
    bool interactive_window_station = true;   ///< WinSta0
};

enum class LockState { Active, Locked, RdpDisconnected, Unknown };

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
};

std::vector<DoctorCheck> run_mcp_doctor(const DoctorInput& input, DoctorProbes& probes);

/// Count of tokens that are neither revoked nor expired (read-only). nullopt when the store cannot be read.
std::optional<int> count_active_tokens(auth::TokenStore& store);

/// "fail" when any check failed, else "warn" when any warned, else "pass".
std::string overall_status(const std::vector<DoctorCheck>& checks);
std::string format_doctor_text(const std::vector<DoctorCheck>& checks);
nlohmann::json doctor_to_json(const std::vector<DoctorCheck>& checks);

} // namespace fairyfly::config
