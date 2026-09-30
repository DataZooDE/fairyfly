#include <iostream>
#include <memory>

#include <CLI/CLI.hpp>

#include "include/commands/mcp_extras.h"
#include "include/console_prompt.h"
#include "include/setup/setup_hosts.h"
#include "include/setup/setup_model.h"
#include "include/setup/setup_service.h"
#include "include/system/powershell_runner.h"

// `fairyfly mcp setup | teardown | cert export`: the one-time machine setup of the http.sys listener (URL reservation,
// TLS binding, certificate, firewall) in the style of erpl-rev: diagnose, plan, consent, apply (one UAC prompt), verify.
// All logic lives in src/setup/ (pure planner + SetupService over host interfaces); this file only parses flags,
// assembles the real hosts and prints. See docs/MCP_SETUP.md.
namespace fairyfly {
namespace commands {

namespace {

using namespace fairyfly::setup;

struct SetupCli {
    CLI::App* setup = nullptr;
    CLI::App* teardown = nullptr;
    CLI::App* cert = nullptr;
    CLI::App* cert_export = nullptr;
    Options options;
    TeardownOptions teardown_options;
    std::string output = "text";
    std::string apply_plan;
    std::string result_file;
    std::string cert_out;
    std::string cert_format = "der";
    std::string cert_hostname;
    int cert_port = 0;
};

struct RealHosts {
    std::unique_ptr<sys::PowerShellRunner> ps = sys::make_windows_powershell_runner();
    std::unique_ptr<HttpSysConfig> http = make_real_http_sys_config();
    std::unique_ptr<CertStore> certs = make_real_cert_store(*ps);
    std::unique_ptr<Firewall> firewall = make_real_firewall(*ps);
    std::unique_ptr<Elevator> elevator = make_real_elevator();
    std::unique_ptr<SystemProbe> sys = make_real_system_probe();
    std::unique_ptr<VerifyHost> verify = make_real_verify_host();
    Hosts hosts() { return Hosts{*http, *certs, *firewall, *elevator, *sys, *verify}; }
};

bool truthy(const std::string& v) { return v == "1" || v == "true" || v == "TRUE" || v == "yes" || v == "on"; }

RunEnv make_env(const McpExtras& x, const GlobalOptions& global) {
    RunEnv env;
    env.stdin_is_tty = cred::stdin_is_console();
    env.read_only = global.read_only || truthy(config::process_env()("FAIRYFLY_READ_ONLY"));
    if (const CLI::Option* ro = x.mcp_app ? x.mcp_app->get_option_no_throw("--read-only") : nullptr; ro && ro->count() > 0) env.read_only = true;
    env.confirm = [](const std::string& prompt) {
        std::cerr << prompt << std::flush;
        std::string line;
        if (!std::getline(std::cin, line)) return false;
        return line == "y" || line == "Y" || line == "yes" || line == "YES" || line == "Yes";
    };
    env.out = &std::cout;
    env.err = &std::cerr;
    return env;
}

std::string quoted(const std::string& s) { return s.find(' ') == std::string::npos ? s : "\"" + s + "\""; }

std::string setup_command_line(const Options& o) {
    std::string cmd = "fairyfly mcp setup";
    if (o.self_signed) cmd += " --self-signed";
    if (!o.thumbprint.empty()) cmd += " --cert-thumbprint " + o.thumbprint;
    if (o.no_tls) cmd += " --no-tls";
    if (!o.hostname.empty() && !o.no_tls) cmd += " --hostname " + o.hostname;
    if (o.port) cmd += " --port " + std::to_string(o.port);
    if (!o.allow_ip.empty()) {
        cmd += " --allow-ip ";
        for (size_t i = 0; i < o.allow_ip.size(); ++i) cmd += (i ? "," : "") + o.allow_ip[i];
    }
    if (!o.user.empty()) cmd += " --user " + quoted(o.user);
    if (o.open_firewall) cmd += " --open-firewall";
    if (o.force_binding) cmd += " --force-binding";
    return cmd + " --yes";
}

std::string teardown_command_line(const TeardownOptions& o) {
    std::string cmd = "fairyfly mcp teardown";
    if (!o.hostname.empty()) cmd += " --hostname " + o.hostname;
    if (o.port) cmd += " --port " + std::to_string(o.port);
    if (o.keep_cert) cmd += " --keep-cert";
    if (o.keep_firewall) cmd += " --keep-firewall";
    return cmd + " --yes";
}

} // namespace

void setup_mcp_setup_commands(CLI::App& mcp, McpExtras& x) {
    auto state = std::make_shared<SetupCli>();
    x.setup_state = state;
    SetupCli& s = *state;
    auto output = [&](CLI::App* cmd) {
        cmd->add_option("--output", s.output, "Output: text (default) or json")->check(CLI::IsMember({"text", "json"}));
    };

    s.setup = mcp.add_subcommand("setup", "One-time machine setup of the https listener: certificate, URL reservation, TLS binding (one UAC prompt)");
    s.setup->fallthrough();
    Options& o = s.options;
    s.setup->add_option("--hostname", o.hostname, "Host name clients use (default: this machine's lower-cased DNS name)");
    s.setup->add_option("--port", o.port, "HTTPS port (default 8443; 8383 with --no-tls)")->check(CLI::Range(1, 65535));
    s.setup->add_flag("--self-signed", o.self_signed, "Create (or reuse) a self-signed certificate 'fairyfly-mcp <host>' in LocalMachine\\My");
    s.setup->add_option("--cert-thumbprint", o.thumbprint, "Use the certificate with this SHA-1 thumbprint from LocalMachine\\My");
    s.setup->add_flag("--no-tls", o.no_tls, "Development: plain HTTP on 127.0.0.1 only (URL reservation, no certificate)");
    s.setup->add_option("--allow-ip", o.allow_ip, "Client addresses/CIDR blocks allowed to connect (written to mcp.yaml; loopback always allowed)")->delimiter(',');
    s.setup->add_option("--user", o.user, "Account that runs the server (DOMAIN\\user; default: the current user)");
    s.setup->add_flag("--open-firewall", o.open_firewall, "Also create the inbound firewall rule 'fairyfly MCP HTTPS <port>'");
    s.setup->add_flag("--force-binding", o.force_binding, "Replace a TLS binding on the port that belongs to another application");
    s.setup->add_flag("--dry-run", o.dry_run, "Print the plan and change nothing");
    s.setup->add_flag("--yes,-y", o.yes, "Apply without the confirmation prompt");
    s.setup->add_flag("--non-interactive", o.non_interactive, "Never prompt (no confirmation, no UAC): fails with CONFIRMATION_REQUIRED or ELEVATION_REQUIRED");
    s.setup->add_flag("--print-runbook", o.print_runbook, "Print the manual equivalent commands and exit");
    output(s.setup);
    s.setup->add_option("--apply-plan", s.apply_plan, "(internal) run an elevated plan file")->group("");
    s.setup->add_option("--result-file", s.result_file, "(internal) result file of --apply-plan")->group("");

    s.teardown = mcp.add_subcommand("teardown", "Remove what 'mcp setup' created (manifest driven, idempotent, one UAC prompt)");
    s.teardown->fallthrough();
    TeardownOptions& t = s.teardown_options;
    s.teardown->add_option("--hostname", t.hostname, "Host name (only needed without a setup manifest)");
    s.teardown->add_option("--port", t.port, "Port (only needed without a setup manifest)")->check(CLI::Range(1, 65535));
    s.teardown->add_flag("--keep-cert", t.keep_cert, "Keep the self-signed certificate and its key");
    s.teardown->add_flag("--keep-firewall", t.keep_firewall, "Keep the firewall rule");
    s.teardown->add_flag("--dry-run", t.dry_run, "Print the plan and change nothing");
    s.teardown->add_flag("--yes,-y", t.yes, "Apply without the confirmation prompt");
    s.teardown->add_flag("--non-interactive", t.non_interactive, "Never prompt (no confirmation, no UAC)");
    output(s.teardown);

    s.cert = mcp.add_subcommand("cert", "Certificate helpers for MCP clients");
    s.cert->fallthrough();
    s.cert->require_subcommand(1);
    s.cert_export = s.cert->add_subcommand("export", "Export the public certificate for clients (with trust hints per platform)");
    s.cert_export->fallthrough();
    s.cert_export->add_option("--out", s.cert_out, "Target file (default %LOCALAPPDATA%\\fairyfly\\fairyfly-mcp-<host>.cer)");
    s.cert_export->add_option("--format", s.cert_format, "der (default) or pem")->check(CLI::IsMember({"der", "pem"}));
    s.cert_export->add_option("--hostname", s.cert_hostname, "Host name (default: from the setup manifest)");
    s.cert_export->add_option("--port", s.cert_port, "Port (default: from the setup manifest)")->check(CLI::Range(1, 65535));
    output(s.cert_export);
}

std::optional<int> run_mcp_setup_commands(McpExtras& x, const GlobalOptions& global) {
    auto* state = static_cast<SetupCli*>(x.setup_state.get());
    if (!state) return std::nullopt;
    SetupCli& s = *state;
    const bool json = s.output == "json";
    const RunEnv env = make_env(x, global);

    if (s.setup && *s.setup) {
        RealHosts real;
        Hosts hosts = real.hosts();
        if (!s.apply_plan.empty()) {
            if (env.read_only) return report_error("text", "READ_ONLY", "FAIRYFLY_READ_ONLY is set: refusing to apply a plan", 2);
            if (s.result_file.empty()) return report_error("text", "INVALID_ARGUMENT", "--apply-plan needs --result-file", 2);
            return run_apply_plan(hosts, s.apply_plan, s.result_file);
        }
        Options o = s.options;
        o.json = json;
        o.config_path = config::resolve_config_path(x.config_path, config::process_env()).path.string();
        fill_defaults(hosts, o);
        o.command_line = setup_command_line(o);
        return run_setup(hosts, o, env);
    }
    if (s.teardown && *s.teardown) {
        RealHosts real;
        Hosts hosts = real.hosts();
        TeardownOptions t = s.teardown_options;
        t.json = json;
        t.config_path = config::resolve_config_path(x.config_path, config::process_env()).path.string();
        t.command_line = teardown_command_line(t);
        return run_teardown(hosts, t, env);
    }
    if (s.cert_export && *s.cert_export) {
        RealHosts real;
        Hosts hosts = real.hosts();
        CertExportOptions c;
        c.out_path = s.cert_out;
        c.format = s.cert_format == "pem" ? CerFormat::Pem : CerFormat::Der;
        c.hostname = s.cert_hostname;
        c.port = s.cert_port;
        c.json = json;
        return run_cert_export(hosts, c, env);
    }
    return std::nullopt;
}

} // namespace commands
} // namespace fairyfly
