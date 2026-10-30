#include "include/doctor_checks.h"

#include "include/string_utils.h"

#include <fmt/format.h>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#endif

namespace fairyfly::cli::doctor {

nlohmann::json to_json(const Check& check) {
    nlohmann::json c;
    c["name"] = check.name;
    c["status"] = check.status;
    c["message"] = check.message;
    if (!check.details.empty()) c["details"] = check.details;
    if (!check.remediation.empty()) c["remediation"] = check.remediation;
    if (!check.code.empty()) c["code"] = check.code;
    return c;
}

std::string overall_health(const std::vector<Check>& checks) {
    bool warned = false;
    for (const auto& c : checks) {
        if (c.status == "fail") return "error";
        warned = warned || c.status == "warn";
    }
    return warned ? "warning" : "ok";
}

Check judge_desktop(const std::string& window_station, const std::string& desktop) {
    const std::string context = window_station + "\\" + desktop;
    if (utils::iequals(window_station, "WinSta0") && utils::iequals(desktop, "Default"))
        return {"desktop_context", "pass", "Running on interactive desktop (" + context + ")"};
    return {"desktop_context", "warn", "Running on non-default desktop: " + context,
            "SAP GUI COM scripting requires access to the interactive desktop.",
            "Ensure the process is running in the active user console session (WinSta0\\Default)."};
}

Check judge_data_dir(const std::string& dir, const std::string& migration_error,
                     const std::vector<config::MigrationConflict>& conflicts) {
    if (dir.empty())
        return {"data_dir", "fail", "No data directory: neither FAIRYFLY_DATA_DIR nor USERPROFILE is set",
                "The audit trail, logs, mcp.yaml and the cloud link pairing live there (docs/DATA_DIR.md).",
                "Run fairyfly in a normal user session, or set FAIRYFLY_DATA_DIR.", "DATA_DIR_UNAVAILABLE"};
    if (!migration_error.empty())
        return {"data_dir", "fail", "Data directory " + dir + ": the old %LOCALAPPDATA%\\fairyfly could not be moved there",
                migration_error, "Check that " + dir + " can be written; fairyfly tries again at the next start.",
                "DATA_DIR_UNAVAILABLE"};
    if (!conflicts.empty()) {
        std::string details;
        for (const auto& c : conflicts)
            details += (details.empty() ? "" : "; ") + c.path + ": " + c.reason + " (kept " + c.kept + ", other copy in " + c.other + ")";
        return {"data_dir", "warn", "Data directory " + dir + ": " + std::to_string(conflicts.size()) +
                                        " file(s) differed between the old folders when they were moved",
                details, "Compare the files named here and keep the right one; the other copy is still in the old folder."};
    }
    return {"data_dir", "pass", "Data directory " + dir};
}

Check judge_notifications(packaging::Package build, const std::string& app_id) {
    if (app_id.empty())
        return {"approval_notifications", "warn",
                "No approval notifications: this Microsoft Store build runs without its package identity",
                "Approval questions of the cloud link open the approval window instead of a notification.",
                "Start fairyfly from the installed package: the Start menu, or `fairyfly` through its alias in a new terminal."};
    if (build == packaging::Package::Msix)
        return {"approval_notifications", "pass", "Approval notifications as " + app_id,
                "The package's tray application; its manifest declares the toast activator (nothing is registered in HKCU)."};
    return {"approval_notifications", "pass", "Approval notifications as " + app_id,
            "Registered in HKCU (AppUserModelId and the toast activator's LocalServer32) when the cloud link starts."};
}

Check judge_processes(const ProcessScan& scan) {
    if (scan.matched > 0) {
        const char* which = scan.logon && scan.gui ? "saplogon.exe and sapgui.exe"
                            : scan.logon           ? "saplogon.exe"
                            : scan.gui             ? "sapgui.exe"
                                                   : "saplgpad.exe";
        return {"sapgui_processes", "pass", fmt::format("SAP GUI process is running ({})", which),
                fmt::format("{} process(es) detected; Toolhelp saw {} entries; {}", scan.matched, scan.toolhelp_count,
                            scan.fallback_attempted ? fmt::format("PSAPI saw {} entries", scan.fallback_count)
                                                    : std::string("PSAPI not run (Toolhelp found SAP GUI)"))};
    }
    return {"sapgui_processes", "warn", "SAP GUI process is not visible to this process",
            fmt::format("Toolhelp saw {} processes (error {}); PSAPI saw {} (error {}).", scan.toolhelp_count,
                        scan.toolhelp_error, scan.fallback_count, scan.fallback_error),
            "Check SAP Logon manually. If it is running, process enumeration may be restricted."};
}

Check judge_client_security(const ClientSecurity& s) {
    if (!s.key_found) return {"client_registry", "pass", "SAP GUI client security registry key using standard defaults"};
    if (s.user_scripting == 0ul)
        return {"client_registry", "fail", "Client scripting is disabled in SAP GUI Security settings",
                "HKCU\\Software\\SAP\\SAPGUI Front\\SAP Frontend Server\\Security\\UserScripting is 0",
                "Open SAP GUI Options -> Accessibility & Scripting -> Scripting -> Check 'Enable scripting'."};
    const unsigned long attach = s.warn_on_attach.value_or(0), connection = s.warn_on_connection.value_or(0);
    if (attach != 0 || connection != 0)
        return {"client_registry", "warn", "Client scripting is enabled, but modal security warnings are active",
                fmt::format("WarnOnAttach={}, WarnOnConnection={}", attach, connection),
                "Uncheck 'Notify when a script attaches to SAP GUI' and 'Notify when a script opens a connection' in SAP GUI "
                "Options to avoid blocking modal dialogs."};
    return {"client_registry", "pass", "Client scripting security configuration is optimal (enabled, modal warnings suppressed)"};
}

Check judge_engine(bool available, bool sap_logon_running, const std::string& detail) {
    if (available) return {"com_engine", "pass", "Connected to SAP Logon's scripting engine"};
    if (!sap_logon_running)
        return {"com_engine", "fail", "SAP Logon is not running", detail,
                "Start SAP Logon (saplogon.exe) in this Windows session and log on; fairyfly finds it at the next call "
                "without a restart.",
                "SAP_LOGON_NOT_RUNNING"};
    return {"com_engine", "fail", "SAP Logon is running, but its scripting engine is not available",
            detail.empty() ? "AutomationEngine returned null" : detail,
            "Enable scripting in the SAP GUI options and verify the SAP GUI Scripting component is installed via SAP "
            "GUI setup."};
}

Check judge_sessions(const nlohmann::json& info) {
    const int connections = info.value("total_connections", 0);
    const int sessions = info.value("total_sessions", 0);
    const bool backend_disabled = info.value("backend_scripting_disabled", false);
    if (info.contains("error"))
        return {"active_sessions", "fail", "Could not enumerate SAP GUI sessions", info["error"].get<std::string>(),
                "Check SAP Logon and retry the diagnostic."};
    if (backend_disabled && sessions == 0)
        return {"active_sessions", "fail", "Backend scripting is disabled on an SAP connection",
                fmt::format("{} connection(s), {} accessible session(s)", connections, sessions),
                "Enable sapgui/user_scripting on the backend and reconnect."};
    if (connections == 0)
        return {"active_sessions", "warn", "No active SAP connections found",
                "SAP GUI scripting engine is active, but no connections are currently open.",
                "Connect to an SAP system in SAP Logon (e.g. Bigfox / A4H) or run 'fairyfly session launch <connection>'."};
    if (const int errors = info.value("connection_enumeration_errors", 0); errors > 0)
        return {"active_sessions", "warn", "Some SAP connections could not be inspected",
                fmt::format("{} accessible session(s), {} connection error(s)", sessions, errors),
                "Inspect the connection error entries from 'fairyfly session list'."};
    if (const int errors = info.value("session_enumeration_errors", 0); errors > 0)
        return {"active_sessions", "warn", "Some SAP sessions could not be inspected",
                fmt::format("{} accessible session(s), {} enumeration error(s)", sessions, errors),
                "Inspect the session_errors entries from 'fairyfly session list'."};
    if (backend_disabled)
        return {"active_sessions", "warn", "An older SAP connection still has backend scripting disabled",
                fmt::format("{} connection(s), {} accessible session(s)", connections, sessions),
                "Reconnect or close the disabled connection; accessible sessions can still be used."};
    if (sessions == 0)
        return {"active_sessions", "warn", "SAP connections are open, but no sessions are accessible",
                fmt::format("{} connection(s), zero accessible sessions", connections),
                "Finish SAP login and verify backend scripting is enabled."};
    return {"active_sessions", "pass",
            fmt::format("Active session verified with backend scripting enabled ({} connections, {} sessions)", connections,
                        sessions)};
}

Check judge_sessions_error(const std::string& what) {
    if (what.find("disabled") != std::string::npos || what.find("Scripting") != std::string::npos)
        return {"active_sessions", "fail", "Backend scripting is disabled by server administrator", what,
                "Enable sapgui/user_scripting = TRUE in transaction RZ11."};
    return {"active_sessions", "warn", fmt::format("Could not query active sessions: {}", what)};
}

// ---- probes ----
#ifdef _WIN32
namespace {
std::string object_name(HANDLE object) {
    char name[256] = {0};
    DWORD length = 0;
    if (object && GetUserObjectInformationA(object, UOI_NAME, name, sizeof(name), &length)) return name;
    return "Unknown";
}

void record(ProcessScan& scan, std::vector<DWORD>& pids, const wchar_t* name, DWORD pid) {
    bool matched = true;
    if (_wcsicmp(name, L"saplogon.exe") == 0) scan.logon = true;
    else if (_wcsicmp(name, L"sapgui.exe") == 0) scan.gui = true;
    else if (_wcsicmp(name, L"saplgpad.exe") == 0) scan.pad = true;
    else matched = false;
    if (matched && std::find(pids.begin(), pids.end(), pid) == pids.end()) pids.push_back(pid);
}

std::optional<unsigned long> read_dword(HKEY key, const wchar_t* value) {
    DWORD data = 0, size = sizeof(DWORD), type = REG_DWORD;
    if (RegQueryValueExW(key, value, nullptr, &type, reinterpret_cast<LPBYTE>(&data), &size) != ERROR_SUCCESS) return std::nullopt;
    return data;
}
}  // namespace

void probe_desktop(std::string& window_station, std::string& desktop) {
    window_station = object_name(GetProcessWindowStation());
    desktop = object_name(GetThreadDesktop(GetCurrentThreadId()));
}

ProcessScan probe_processes() {
    ProcessScan scan;
    std::vector<DWORD> pids;
    HANDLE snapshot = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 3; ++attempt) {
        snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
        if (snapshot != INVALID_HANDLE_VALUE) break;
        scan.toolhelp_error = GetLastError();
        if (scan.toolhelp_error != ERROR_BAD_LENGTH) break;
    }
    if (snapshot != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{};
        pe.dwSize = sizeof(pe);
        if (Process32FirstW(snapshot, &pe)) {
            do {
                ++scan.toolhelp_count;
                record(scan, pids, pe.szExeFile, pe.th32ProcessID);
            } while (Process32NextW(snapshot, &pe));
            const DWORD walk_error = GetLastError();
            if (walk_error != ERROR_NO_MORE_FILES) scan.toolhelp_error = walk_error;
        } else {
            scan.toolhelp_error = GetLastError();
        }
        CloseHandle(snapshot);
    }
    // Toolhelp may expose only a restricted process snapshot. Fall back to PSAPI.
    if (pids.empty()) {
        scan.fallback_attempted = true;
        std::vector<DWORD> ids(1024);
        DWORD bytes = 0;
        bool enumerated = false;
        while (ids.size() <= 16384) {
            if (!K32EnumProcesses(ids.data(), static_cast<DWORD>(ids.size() * sizeof(DWORD)), &bytes)) {
                scan.fallback_error = GetLastError();
                break;
            }
            if (bytes < ids.size() * sizeof(DWORD)) {
                enumerated = true;
                break;
            }
            ids.resize(ids.size() * 2);
        }
        if (enumerated) {
            scan.fallback_count = bytes / sizeof(DWORD);
            for (std::size_t i = 0; i < scan.fallback_count; ++i) {
                HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, ids[i]);
                if (!process) continue;
                wchar_t image[MAX_PATH] = {};
                DWORD length = MAX_PATH;
                if (QueryFullProcessImageNameW(process, 0, image, &length)) {
                    const wchar_t* file = wcsrchr(image, L'\\');
                    record(scan, pids, file ? file + 1 : image, ids[i]);
                }
                CloseHandle(process);
            }
        }
    }
    scan.matched = pids.size();
    if (scan.matched == 0)
        spdlog::debug("SAP process scan: Toolhelp saw {} entries (error {}), PSAPI saw {} (error {})", scan.toolhelp_count,
                      scan.toolhelp_error, scan.fallback_count, scan.fallback_error);
    return scan;
}

ClientSecurity probe_client_security() {
    ClientSecurity s;
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\SAP\\SAPGUI Front\\SAP Frontend Server\\Security", 0, KEY_READ, &key) !=
        ERROR_SUCCESS)
        return s;
    s.key_found = true;
    s.user_scripting = read_dword(key, L"UserScripting");
    s.warn_on_attach = read_dword(key, L"WarnOnAttach");
    s.warn_on_connection = read_dword(key, L"WarnOnConnection");
    RegCloseKey(key);
    return s;
}
#else
void probe_desktop(std::string& window_station, std::string& desktop) {
    window_station = "Unknown";
    desktop = "Unknown";
}
ProcessScan probe_processes() { return {}; }
ClientSecurity probe_client_security() { return {}; }
#endif

}  // namespace fairyfly::cli::doctor
