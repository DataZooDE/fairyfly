#pragma once
// `fairyfly doctor`: each check is a probe (what the desktop, the process list or the registry say; Win32) and a
// judge (what that means; pure, unit-tested). CommandHandler::handle_doctor runs the probes and collects the judges.
#include <nlohmann/json.hpp>

#include "include/config/data_dir.h"

#include <cstddef>
#include <optional>
#include <string>
#include <vector>

namespace fairyfly::cli::doctor {

/// One line of the report. status: pass | warn | fail | info.
struct Check {
    std::string name;
    std::string status;
    std::string message;
    std::string details;
    std::string remediation;
    std::string code;  ///< the error code a command would answer for the same cause (e.g. SAP_LOGON_NOT_RUNNING)
};

nlohmann::json to_json(const Check& check);
/// error (a fail), warning (a warn) or ok.
std::string overall_health(const std::vector<Check>& checks);

// ---- judges ----
Check judge_desktop(const std::string& window_station, const std::string& desktop);

/// The data directory (docs/DATA_DIR.md): fail (DATA_DIR_UNAVAILABLE) without one or when this start could not move
/// the old folder there (`migration_error`); warn for the conflicts recorded when the old folders were merged.
Check judge_data_dir(const std::string& dir, const std::string& migration_error,
                     const std::vector<config::MigrationConflict>& conflicts);

struct ProcessScan {
    bool logon = false;  ///< saplogon.exe
    bool gui = false;    ///< sapgui.exe
    bool pad = false;    ///< saplgpad.exe
    std::size_t matched = 0;          ///< SAP GUI processes found
    std::size_t toolhelp_count = 0;   ///< processes Toolhelp listed
    unsigned long toolhelp_error = 0;
    bool fallback_attempted = false;  ///< PSAPI ran (Toolhelp found no SAP GUI)
    std::size_t fallback_count = 0;
    unsigned long fallback_error = 0;
};
Check judge_processes(const ProcessScan& scan);

/// HKCU\Software\SAP\SAPGUI Front\SAP Frontend Server\Security
struct ClientSecurity {
    bool key_found = false;
    std::optional<unsigned long> user_scripting;
    std::optional<unsigned long> warn_on_attach;
    std::optional<unsigned long> warn_on_connection;
};
Check judge_client_security(const ClientSecurity& security);

/// SAP Logon's scripting engine (AutomationEngine::engine_status): pass only when the engine holds the running SAP
/// Logon's GuiApplication; "SAP Logon is not running" when there is no SAPGUI entry in the running object table.
Check judge_engine(bool available, bool sap_logon_running = true, const std::string& detail = "");
/// From AutomationEngine::get_application_info().
Check judge_sessions(const nlohmann::json& application_info);
/// get_application_info threw.
Check judge_sessions_error(const std::string& what);

// ---- probes (Windows; elsewhere they report "unknown") ----
/// Name of this process's window station and of its thread's desktop ("Unknown" when it cannot be read).
void probe_desktop(std::string& window_station, std::string& desktop);
ProcessScan probe_processes();
ClientSecurity probe_client_security();

}  // namespace fairyfly::cli::doctor
