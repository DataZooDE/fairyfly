#include "include/commands/mcp_iis_command.h"

#include <windows.h>
#include <bcrypt.h>

#include <cstdlib>
#include <ctime>
#include <memory>

#include "include/credential_store.h"
#include "include/iis/iis_service.h"
#include "include/iis/powershell_host.h"

namespace fairyfly {
namespace commands {
namespace {

struct IisCliState {
    CLI::App* iis = nullptr;
    CLI::App* setup = nullptr;
    CLI::App* status = nullptr;
    CLI::App* remove = nullptr;
    iis::SetupOptions setup_options;
    iis::StatusOptions status_options;
    iis::RemoveOptions remove_options;
};

IisCliState& state() {
    static IisCliState s;
    return s;
}

std::string random_secret_hex() {
    unsigned char bytes[32];
    if (BCryptGenRandom(nullptr, bytes, sizeof bytes, BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
        throw iis::HostError{"IIS_RANDOM_FAILED", "the system random number generator failed"};
    }
    static const char* digits = "0123456789abcdef";
    std::string out;
    for (const unsigned char b : bytes) {
        out += digits[b >> 4];
        out += digits[b & 15];
    }
    SecureZeroMemory(bytes, sizeof bytes);
    return out;
}

Result to_result(const iis::Report& report) {
    Result result;
    if (report.ok) {
        result.status = Result::Status::Success;
        result.data = report.data;
    } else {
        result.status = Result::Status::Error;
        result.error = report.data.is_object() ? report.data : nlohmann::json::object();
        result.error["code"] = report.error_code;
        result.error["message"] = report.error_message;
    }
    return result;
}

Result error_result(const std::string& code, const std::string& message) {
    Result result;
    result.status = Result::Status::Error;
    result.error["code"] = code;
    result.error["message"] = message;
    return result;
}

bool read_only_env() {
    char buf[8] = {};
    const DWORD n = GetEnvironmentVariableA("FAIRYFLY_READ_ONLY", buf, sizeof buf);
    return n == 1 && buf[0] == '1';
}

} // namespace

void register_mcp_iis_cli(CLI::App& mcp_app) {
    IisCliState& s = state();
    s = IisCliState{};
    s.iis = mcp_app.add_subcommand("iis", "Front fairyfly with an IIS reverse proxy (setup, status, remove; elevated)");
    s.iis->require_subcommand(1);
    s.iis->fallthrough();

    s.setup = s.iis->add_subcommand("setup", "Create the IIS site, certificate binding, web.config and proxy secret");
    s.setup->fallthrough();
    auto& o = s.setup_options;
    s.setup->add_option("--hostname", o.hostname, "Public DNS name clients use (required)");
    s.setup->add_option("--port", o.port, "HTTPS port (default 8443)");
    s.setup->add_option("--upstream", o.upstream, "fairyfly HTTP server, loopback only (default http://127.0.0.1:8383)");
    s.setup->add_option("--site-name", o.site_name, "IIS site and app pool name (default fairyfly-mcp)");
    s.setup->add_option("--site-path", o.site_path, "Site directory (default C:\\inetpub\\fairyfly-mcp)");
    s.setup->add_option("--cert-thumbprint", o.cert_thumbprint, "Thumbprint of an existing LocalMachine\\My certificate");
    s.setup->add_flag("--self-signed", o.self_signed, "Create a self-signed certificate (clients must trust the exported .cer)");
    s.setup->add_option("--allow-ip", o.allow_ips, "Client IPs/CIDRs allowed by IIS (comma separated)")->delimiter(',');
    s.setup->add_flag("--allow-any-ip", o.allow_any_ip, "Disable the IIS IP restriction (not recommended)");
    s.setup->add_flag("--rotate-secret", o.rotate_secret, "Generate a new proxy secret");
    s.setup->add_flag("--open-firewall", o.open_firewall, "Add a Windows Firewall rule for the HTTPS port");
    s.setup->add_flag("--dry-run", o.dry_run, "Only print the plan; change nothing");
    s.setup->add_flag("--yes", o.yes, "Confirm the changes");

    s.status = s.iis->add_subcommand("status", "Check site, binding, certificate expiry, web.config drift and the upstream");
    s.status->fallthrough();
    s.status->add_option("--site-name", s.status_options.site_name, "IIS site name (default fairyfly-mcp)");
    s.status->add_option("--site-path", s.status_options.site_path, "Site directory (default C:\\inetpub\\fairyfly-mcp)");

    s.remove = s.iis->add_subcommand("remove", "Remove the site, app pool and web.config");
    s.remove->fallthrough();
    auto& r = s.remove_options;
    s.remove->add_option("--site-name", r.site_name, "IIS site name (default fairyfly-mcp)");
    s.remove->add_option("--site-path", r.site_path, "Site directory (default C:\\inetpub\\fairyfly-mcp)");
    s.remove->add_flag("--remove-cert", r.remove_cert, "Also delete the self-signed certificate created by setup");
    s.remove->add_flag("--remove-firewall", r.remove_firewall, "Also delete the firewall rule added by setup");
    s.remove->add_flag("--delete-secret", r.delete_secret, "Also delete the stored proxy secret");
    s.remove->add_flag("--dry-run", r.dry_run, "Only print the plan; change nothing");
    s.remove->add_flag("--yes", r.yes, "Confirm the removal");
}

bool mcp_iis_invoked() {
    const IisCliState& s = state();
    return s.iis && *s.iis;
}

Result mcp_iis_execute() {
    IisCliState& s = state();
    const bool is_setup = s.setup && *s.setup;
    const bool is_status = s.status && *s.status;
    const bool is_remove = s.remove && *s.remove;
    if (!is_setup && !is_status && !is_remove) return error_result("NO_SUBCOMMAND", "mcp iis needs a verb: setup, status or remove");

    const bool dry = (is_setup && s.setup_options.dry_run) || (is_remove && s.remove_options.dry_run);
    if (!is_status && !dry && read_only_env()) {
        return error_result("READ_ONLY", "FAIRYFLY_READ_ONLY=1 forbids changing IIS; use --dry-run to see the plan");
    }

    try {
        auto runner = iis::make_windows_powershell_runner();
        auto host = iis::make_powershell_host(*runner);
        auto store = cred::make_default_store();
        iis::IisContext ctx{*host, *store, random_secret_hex, [] { return static_cast<int64_t>(std::time(nullptr)); }};
        if (is_setup) return to_result(iis::run_setup(ctx, s.setup_options));
        if (is_status) return to_result(iis::run_status(ctx, s.status_options));
        return to_result(iis::run_remove(ctx, s.remove_options));
    } catch (const iis::HostError& e) {
        return error_result(e.code, e.message);
    } catch (const cred::CredentialError& e) {
        return error_result(e.code(), e.what());
    } catch (const std::exception& e) {
        return error_result("IIS_ERROR", e.what());
    }
}

} // namespace commands
} // namespace fairyfly
