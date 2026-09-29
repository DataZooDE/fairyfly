#include "include/config/mcp_doctor.h"

#include <algorithm>
#include <cctype>

namespace fairyfly::config {

namespace {

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

DoctorCheck check(std::string id, CheckStatus status, std::string message, std::string remediation = {}) {
    return {std::move(id), status, std::move(message), std::move(remediation)};
}

} // namespace

const char* check_status_name(CheckStatus status) {
    switch (status) {
    case CheckStatus::Pass: return "pass";
    case CheckStatus::Warn: return "warn";
    case CheckStatus::Fail: return "fail";
    default: return "skip";
    }
}

std::vector<DoctorCheck> run_mcp_doctor(const DoctorInput& in, DoctorProbes& probes) {
    std::vector<DoctorCheck> out;
    const bool http = in.transport == "http";

    // 1. config
    {
        const bool has_error = std::any_of(in.config.issues.begin(), in.config.issues.end(),
                                           [](const ConfigIssue& i) { return i.severity == Severity::Error; });
        size_t warnings = 0;
        const ConfigIssue* first_error = nullptr;
        for (const auto& i : in.config.issues) {
            if (i.severity == Severity::Warning) ++warnings;
            else if (!first_error) first_error = &i;
        }
        if (in.config.read_error) {
            out.push_back(check("config", CheckStatus::Fail, "config file cannot be read: " + in.config.path,
                                "Check the path and permissions."));
        } else if (has_error) {
            out.push_back(check("config", CheckStatus::Fail, format_issue(*first_error, in.config.path),
                                "Fix the file; run 'fairyfly mcp config validate' for all problems."));
        } else if (!in.config.exists) {
            out.push_back(check("config", CheckStatus::Pass, "no config file (" + in.config.path + "); defaults apply",
                                "Optional: 'fairyfly mcp config init' writes a commented template."));
        } else if (warnings > 0) {
            out.push_back(check("config", CheckStatus::Warn, in.config.path + " is valid with " + std::to_string(warnings) + " warning(s)",
                                "Run 'fairyfly mcp config validate' to see them."));
        } else {
            out.push_back(check("config", CheckStatus::Pass, in.config.path + " is valid"));
        }
    }

    // 2. port
    if (!http) {
        out.push_back(check("port", CheckStatus::Skip, "stdio transport, no port used"));
    } else {
        const std::string where = in.host + ":" + std::to_string(in.port);
        const bool tray = probes.tray_running();
        switch (probes.probe_port(in.host, in.port)) {
        case PortState::Free:
            out.push_back(check("port", CheckStatus::Pass, where + " is free"));
            break;
        case PortState::InUse:
            if (tray) out.push_back(check("port", CheckStatus::Pass, where + " is bound (the tray server appears to be running)"));
            else out.push_back(check("port", CheckStatus::Warn, where + " is already in use by another process",
                                     "Stop the other process or choose another server.port."));
            break;
        default:
            out.push_back(check("port", CheckStatus::Skip, "could not probe " + where));
        }
    }

    // 3/4. SAP GUI scripting and a logged-in session
    {
        const SapState sap = probes.sap();
        switch (sap.scripting_available) {
        case SapState::Tri::Yes:
            out.push_back(check("sap_scripting", CheckStatus::Pass, "SAP GUI scripting is available"));
            break;
        case SapState::Tri::No:
            out.push_back(check("sap_scripting", CheckStatus::Fail, "SAP GUI scripting is not available" + (sap.detail.empty() ? "" : ": " + sap.detail),
                                "Start SAP Logon and enable scripting (client and server); see 'fairyfly doctor'."));
            break;
        default:
            out.push_back(check("sap_scripting", CheckStatus::Skip, "could not determine SAP GUI scripting availability"));
        }
        switch (sap.session_present) {
        case SapState::Tri::Yes:
            out.push_back(check("sap_session", CheckStatus::Pass, "a logged-in SAP session is present"));
            break;
        case SapState::Tri::No:
            out.push_back(check("sap_session", CheckStatus::Warn, "no logged-in SAP session",
                                "Log on (fairyfly session launch <name> --login) before clients call tools."));
            break;
        default:
            out.push_back(check("sap_session", CheckStatus::Skip, "session state unknown"));
        }
    }

    // 5. interactive desktop
    {
        const DesktopState d = probes.desktop();
        if (d.session_id && *d.session_id == 0) {
            out.push_back(check("desktop", CheckStatus::Fail, "running in Session 0 (non-interactive, e.g. a service or scheduled task)",
                                "SAP GUI scripting needs the logged-on user's interactive desktop: start fairyfly from that user's session."));
        } else if (!d.interactive_window_station) {
            out.push_back(check("desktop", CheckStatus::Warn, "not on the interactive window station (WinSta0)",
                                "Run from the user's desktop session."));
        } else if (!d.session_id) {
            out.push_back(check("desktop", CheckStatus::Skip, "session id unknown"));
        } else {
            out.push_back(check("desktop", CheckStatus::Pass, "interactive desktop (session " + std::to_string(*d.session_id) + ")"));
        }
    }

    // 6. locked / RDP disconnected
    switch (probes.lock_state()) {
    case LockState::Locked:
        out.push_back(check("session_lock", CheckStatus::Warn, "the workstation is locked: screenshots will be black and some controls unreachable",
                            "Unlock it, disable the lock screen, or keep the session on the console (see docs/MCP_TRAY.md)."));
        break;
    case LockState::RdpDisconnected:
        out.push_back(check("session_lock", CheckStatus::Warn, "the session is disconnected (RDP): screenshots will be black",
                            "Use 'tscon <id> /dest:console' when leaving, or use autologon on the console (see docs/MCP_TRAY.md)."));
        break;
    case LockState::Active:
        out.push_back(check("session_lock", CheckStatus::Pass, "session is active and unlocked"));
        break;
    default:
        out.push_back(check("session_lock", CheckStatus::Skip, "lock/RDP state unknown"));
    }

    // 7. tokens
    {
        const auto count = probes.token_count();
        if (!count) {
            out.push_back(check("tokens", CheckStatus::Skip, "token count unknown (token store not available in this build)"));
        } else if (*count == 0 && http) {
            out.push_back(check("tokens", CheckStatus::Warn, "no tokens exist: the HTTP server answers every request with 401",
                                "Create one with 'fairyfly mcp token create'."));
        } else {
            out.push_back(check("tokens", CheckStatus::Pass, std::to_string(*count) + " token(s)"));
        }
    }

    // 8. IIS
    {
        const IisState iis = probes.iis();
        if (!iis.checked) out.push_back(check("iis", CheckStatus::Skip, "not checked"));
        else if (iis.ok) out.push_back(check("iis", CheckStatus::Pass, iis.message.empty() ? "IIS reverse proxy is configured" : iis.message));
        else out.push_back(check("iis", CheckStatus::Warn, iis.message.empty() ? "IIS reverse proxy problem" : iis.message,
                                 "Run 'fairyfly mcp iis status' (elevated) for details."));
    }

    // 9. autostart
    {
        const auto command = probes.autostart_command();
        if (!command) {
            out.push_back(check("autostart", CheckStatus::Skip, "not registered (optional: 'fairyfly mcp --tray --install-autostart')"));
        } else if (!in.exe_path.empty() && lower(*command).find(lower(in.exe_path)) == std::string::npos) {
            out.push_back(check("autostart", CheckStatus::Warn, "registered for a different fairyfly.exe: " + *command,
                                "Run '--install-autostart' again from the current install."));
        } else {
            out.push_back(check("autostart", CheckStatus::Pass, "registered: " + *command));
        }
    }

    // 10. tray
    out.push_back(probes.tray_running() ? check("tray", CheckStatus::Pass, "tray is running")
                                        : check("tray", CheckStatus::Skip, "tray is not running"));
    return out;
}

std::string overall_status(const std::vector<DoctorCheck>& checks) {
    bool warn = false;
    for (const auto& c : checks) {
        if (c.status == CheckStatus::Fail) return "fail";
        if (c.status == CheckStatus::Warn) warn = true;
    }
    return warn ? "warn" : "pass";
}

std::string format_doctor_text(const std::vector<DoctorCheck>& checks) {
    std::string out;
    for (const auto& c : checks) {
        std::string tag = lower(check_status_name(c.status));
        for (auto& ch : tag) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        out += "[" + tag + "] " + c.id + ": " + c.message + "\n";
        if (!c.remediation.empty() && (c.status == CheckStatus::Warn || c.status == CheckStatus::Fail))
            out += "       -> " + c.remediation + "\n";
    }
    out += "overall: " + overall_status(checks) + "\n";
    return out;
}

nlohmann::json doctor_to_json(const std::vector<DoctorCheck>& checks) {
    nlohmann::json out;
    out["overall"] = overall_status(checks);
    out["checks"] = nlohmann::json::array();
    for (const auto& c : checks) {
        nlohmann::json j{{"id", c.id}, {"status", check_status_name(c.status)}, {"message", c.message}};
        if (!c.remediation.empty()) j["remediation"] = c.remediation;
        out["checks"].push_back(std::move(j));
    }
    return out;
}

} // namespace fairyfly::config
