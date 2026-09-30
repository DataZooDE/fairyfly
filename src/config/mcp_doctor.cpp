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

std::optional<int> count_active_tokens(auth::TokenStore& store) {
    try {
        const auto now = store.now();
        int count = 0;
        for (const auto& token : store.list()) {
            if (token.revoked) continue;
            if (token.expires && *token.expires <= now) continue;
            ++count;
        }
        return count;
    } catch (...) {
        return std::nullopt;
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
            out.push_back(check("tokens", CheckStatus::Skip, "token count unknown (token store could not be read)"));
        } else if (*count == 0 && http) {
            out.push_back(check("tokens", CheckStatus::Warn, "no tokens exist: the HTTP server answers every request with 401",
                                "Create one with 'fairyfly mcp token create'."));
        } else {
            out.push_back(check("tokens", CheckStatus::Pass, std::to_string(*count) + " token(s)"));
        }
    }

    // 7b. http.sys setup (read-only facts; never elevated)
    {
        SetupQuery query;
        query.tls = in.tls;
        query.hostname = in.hostname;
        query.port = in.port;
        const SetupFacts f = probes.setup_facts(query);
        const bool relevant = f.available && (http || f.manifest_present);
        const std::string skip_reason = f.available ? "stdio transport, no http.sys setup needed" : "setup state not collected";
        const std::string tls_flag = in.tls ? "--self-signed" : "--no-tls";
        const std::string fix = "Run 'fairyfly mcp setup " + tls_flag + "' (see docs/MCP_SETUP.md).";
        const bool tls = f.manifest_present ? f.manifest_mode == "tls" : in.tls;

        // elevation (info)
        if (!f.available || f.elevation.empty() || f.elevation == "unknown") out.push_back(check("elevation", CheckStatus::Skip, "elevation state unknown"));
        else out.push_back(check("elevation", CheckStatus::Pass, f.elevation + (f.elevation == "elevated" ? " (doctor itself never needs it)" : "")));

        // setup_manifest
        if (!relevant) out.push_back(check("setup_manifest", CheckStatus::Skip, skip_reason));
        else if (f.manifest_unreadable) out.push_back(check("setup_manifest", CheckStatus::Warn, "the setup manifest is unreadable: " + f.manifest_path, "Run 'fairyfly mcp teardown --hostname H --port P' and set up again."));
        else if (!f.manifest_present) out.push_back(check("setup_manifest", CheckStatus::Warn, "no setup recorded (" + f.manifest_path + ")", fix));
        else out.push_back(check("setup_manifest", CheckStatus::Pass, "setup for " + f.manifest_hostname + ":" + std::to_string(f.manifest_port) + " (" + f.manifest_mode + ")"));

        // urlacl
        if (!relevant) out.push_back(check("urlacl", CheckStatus::Skip, skip_reason));
        else if (!f.urlacl_known) out.push_back(check("urlacl", CheckStatus::Skip, "could not read the URL reservation of " + f.prefix));
        else if (!f.urlacl_reserved && !tls) out.push_back(check("urlacl", CheckStatus::Pass, "not required for the loopback prefix " + f.prefix + " on this Windows build"));
        else if (!f.urlacl_reserved) out.push_back(check("urlacl", CheckStatus::Fail, f.prefix + " is not reserved: the server cannot listen without elevation", fix));
        else if (!f.urlacl_covers_user && tls) out.push_back(check("urlacl", CheckStatus::Fail, f.prefix + " is reserved, but not for the current user", "Run 'fairyfly mcp setup " + tls_flag + "' as this user (it adds the user to the reservation) or use --user."));
        else out.push_back(check("urlacl", CheckStatus::Pass, f.prefix + " is reserved for the current user"));

        // sslcert
        if (!relevant) out.push_back(check("sslcert", CheckStatus::Skip, skip_reason));
        else if (!tls) out.push_back(check("sslcert", CheckStatus::Skip, "plain HTTP mode: no TLS binding"));
        else if (!f.ssl_known) out.push_back(check("sslcert", CheckStatus::Skip, "could not read the TLS bindings"));
        else if (f.ssl_foreign) out.push_back(check("sslcert", CheckStatus::Warn, "port " + std::to_string(in.port) + " is bound by another application", "Free the binding or re-run setup with --force-binding."));
        else if (!f.ssl_v4) out.push_back(check("sslcert", CheckStatus::Fail, "no certificate is bound to 0.0.0.0:" + std::to_string(in.port), fix));
        else if (!f.cert_found && f.cert_known) out.push_back(check("sslcert", CheckStatus::Fail, "the bound certificate " + f.ssl_thumbprint + " is not in LocalMachine\\My", fix));
        else if (!f.manifest_thumbprint.empty() && f.manifest_thumbprint != f.ssl_thumbprint) out.push_back(check("sslcert", CheckStatus::Warn, "the bound certificate differs from the one recorded by setup", "Run 'fairyfly mcp setup " + tls_flag + "' to rebind."));
        else if (!f.ssl_v6) out.push_back(check("sslcert", CheckStatus::Warn, "bound on 0.0.0.0:" + std::to_string(in.port) + " but not on [::]:" + std::to_string(in.port) + " (IPv6 clients cannot connect)", fix));
        else out.push_back(check("sslcert", CheckStatus::Pass, "certificate " + f.ssl_thumbprint.substr(0, 8) + "... bound on 0.0.0.0 and [::]"));

        // certificate
        if (!relevant) out.push_back(check("certificate", CheckStatus::Skip, skip_reason));
        else if (!tls) out.push_back(check("certificate", CheckStatus::Skip, "plain HTTP mode: no certificate"));
        else if (!f.cert_known) out.push_back(check("certificate", CheckStatus::Skip, "could not read LocalMachine\\My"));
        else if (!f.cert_found) out.push_back(check("certificate", CheckStatus::Fail, "no certificate for this server in LocalMachine\\My", fix));
        else if (!f.cert_has_key) out.push_back(check("certificate", CheckStatus::Fail, "certificate " + f.cert_thumbprint + " has no private key", "Import the certificate with its private key into LocalMachine\\My."));
        else if (f.cert_not_after <= f.now) out.push_back(check("certificate", CheckStatus::Fail, "certificate " + f.cert_thumbprint + " expired", "Run 'fairyfly mcp teardown' then 'fairyfly mcp setup " + tls_flag + "' to issue a new one (clients must trust it again)."));
        else if (!f.cert_san_ok) out.push_back(check("certificate", CheckStatus::Fail, "certificate " + f.cert_thumbprint + " does not cover the host name" + (in.hostname.empty() ? std::string() : " '" + in.hostname + "'"), "Set up again with --hostname matching the certificate."));
        else if ((f.cert_not_after - f.now) < 30LL * 86400) out.push_back(check("certificate", CheckStatus::Warn, "certificate expires in " + std::to_string((f.cert_not_after - f.now) / 86400) + " days", "Plan a renewal: 'fairyfly mcp teardown' then 'fairyfly mcp setup " + tls_flag + "'."));
        else out.push_back(check("certificate", CheckStatus::Pass, "certificate " + f.cert_thumbprint.substr(0, 8) + "... valid for " + std::to_string((f.cert_not_after - f.now) / 86400) + " more days, private key present"));

        // firewall (only when setup recorded a rule)
        if (!relevant || f.manifest_firewall_rule.empty()) out.push_back(check("firewall", CheckStatus::Skip, "no firewall rule recorded by setup"));
        else if (!f.firewall_known) out.push_back(check("firewall", CheckStatus::Skip, "could not read the firewall rule"));
        else if (!f.firewall_exists) out.push_back(check("firewall", CheckStatus::Warn, "the firewall rule '" + f.manifest_firewall_rule + "' recorded by setup is gone", "Run 'fairyfly mcp setup " + tls_flag + " --open-firewall'."));
        else out.push_back(check("firewall", CheckStatus::Pass, "rule '" + f.manifest_firewall_rule + "' exists"));

        // tls_handshake (only when something listens)
        if (!relevant || !tls) out.push_back(check("tls_handshake", CheckStatus::Skip, relevant ? "plain HTTP mode" : skip_reason));
        else if (!f.port_listening || f.tls_status.empty()) out.push_back(check("tls_handshake", CheckStatus::Skip, "nothing listens on port " + std::to_string(in.port) + " (start 'fairyfly mcp --http')"));
        else if (f.tls_status != "ok") out.push_back(check("tls_handshake", CheckStatus::Warn, "TLS handshake failed (" + f.tls_status + ")", "Check the TLS binding and the certificate."));
        else if (!f.tls_thumbprint_match) out.push_back(check("tls_handshake", CheckStatus::Warn, "the server presents a different certificate than the one installed", "Rebind with 'fairyfly mcp setup " + tls_flag + "' and restart the server."));
        else if (f.tls_protocol == "TLS 1.0" || f.tls_protocol == "TLS 1.1" || f.tls_protocol.rfind("SSL", 0) == 0) out.push_back(check("tls_handshake", CheckStatus::Warn, "negotiated " + f.tls_protocol, "Raise the machine Schannel policy to TLS 1.2 or newer."));
        else out.push_back(check("tls_handshake", CheckStatus::Pass, "negotiated " + (f.tls_protocol.empty() ? std::string("TLS") : f.tls_protocol) + ", certificate matches"));

        // legacy proxy secret (retired reverse-proxy design)
        if (probes.legacy_proxy_secret())
            out.push_back(check("legacy_proxy_secret", CheckStatus::Warn, "the retired proxy secret 'fairyfly:fairyfly-mcp-proxy' is still stored", "cmdkey /delete:fairyfly:fairyfly-mcp-proxy"));
        else out.push_back(check("legacy_proxy_secret", CheckStatus::Pass, "no legacy proxy secret"));
    }

    // 8. autostart
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

    // 9. tray
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
