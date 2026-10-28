#include <chrono>
#include "include/config/environment.h"
#include "include/mcp/constants.h"
#include <iostream>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "include/audit_log.h"
#include "include/commands/mcp_extras.h"
#include "include/link/link_runner.h"
#include "include/packaging.h"
#include "include/update/updater.h"
#include "include/version.h"
#include "include/tray/mcp_tray.h"
#include "include/tray/tray_win32.h"

namespace fairyfly {
namespace commands {

using nlohmann::json;
using namespace fairyfly::config;

namespace {

CLI::Option* output_option(CLI::App* cmd, std::string& target) {
    return cmd->add_option("--output", target, "Output: text (default) or json")
        ->check(CLI::IsMember({"text", "json", "markdown"}));
}

/// The option's value as the key table parses it: a list joins every occurrence, a scalar takes what CLI11 bound
/// (the last of a repeated flag, so `--tls --no-tls` is false, as in ServeOptions).
std::string flag_text(const KeySpec& spec, const CLI::Option& option) {
    const auto& results = spec.type == ValueType::StringList ? option.results() : option.reduced_results();
    std::string out;
    for (const auto& r : results) out += (out.empty() ? "" : ",") + r;
    return out;
}

void print_json(const json& j) { std::cout << j.dump(2) << std::endl; }

json issues_json(const std::vector<ConfigIssue>& issues) {
    json out = json::array();
    for (const auto& i : issues)
        out.push_back({{"severity", i.severity == Severity::Error ? "error" : "warning"}, {"code", i.code},
                       {"message", i.message}, {"line", i.line}, {"key", i.key}});
    return out;
}

int cmd_path(McpExtras& x) {
    const auto env = process_env();
    const ConfigPath path = resolve_config_path(x.config_path, env);
    const bool exists = std::filesystem::exists(path.path);
    if (x.output == "json") {
        print_json({{"status", "success"}, {"data", {{"path", path.path.string()}, {"source", source_name(path.source)}, {"exists", exists}}}});
    } else {
        std::cout << path.path.string() << "\n(" << source_name(path.source) << ", " << (exists ? "exists" : "does not exist") << ")\n";
    }
    return 0;
}

int cmd_init(McpExtras& x) {
    const ConfigPath path = resolve_config_path(x.config_path, process_env());
    std::error_code ec;
    if (std::filesystem::exists(path.path, ec) && !x.init_force)
        return report_error(x.output, "CONFIG_EXISTS", "Config file already exists: " + path.path.string() + " (use --force to overwrite)");
    if (!write_text_file(path.path, config_template()))
        return report_error(x.output, "CONFIG_WRITE_FAILED", "Cannot write " + path.path.string());
    if (x.output == "json") print_json({{"status", "success"}, {"data", {{"path", path.path.string()}, {"written", true}}}});
    else std::cout << "Wrote " << path.path.string() << "\nEdit it, then run: fairyfly mcp config validate\n";
    return 0;
}

int cmd_validate(McpExtras& x) {
    const LoadedConfig loaded = load_config(x.config_path, process_env());
    const std::string label = loaded.path.path.string();
    if (loaded.read_error) return report_error(x.output, "CONFIG_READ_FAILED", "Cannot read " + label);
    if (!loaded.exists)
        return report_error(x.output, "CONFIG_NOT_FOUND", "No config file at " + label + " (create one with: fairyfly mcp config init)");
    const bool ok = loaded.parsed.ok();
    if (x.output == "json") {
        print_json({{"status", ok ? "success" : "error"}, {"data", {{"path", label}, {"valid", ok}, {"issues", issues_json(loaded.parsed.issues)}}}});
    } else {
        for (const auto& i : loaded.parsed.issues) std::cout << format_issue(i, label) << "\n";
        std::cout << label << (ok ? " is valid\n" : " is INVALID\n");
    }
    return ok ? 0 : 1;
}

int cmd_show(McpExtras& x) {
    const auto env = process_env();
    const LoadedConfig loaded = load_config(x.config_path, env);
    const std::string label = loaded.path.path.string();
    if (loaded.read_error) return report_error(x.output, "CONFIG_READ_FAILED", "Cannot read " + label);
    if (loaded.path.explicit_request && !loaded.exists)
        return report_error(x.output, "CONFIG_NOT_FOUND", "Config file not found: " + label);
    if (!loaded.parsed.ok()) {
        if (x.output == "json") print_json({{"status", "error"}, {"error", {{"code", loaded.parsed.first_error_code()}, {"message", label + " is invalid"}}}, {"issues", issues_json(loaded.parsed.issues)}});
        else for (const auto& i : loaded.parsed.issues) std::cerr << format_issue(i, label) << "\n";
        return 1;
    }
    std::vector<ConfigIssue> flag_issues;
    const McpConfig flags = flags_layer(*x.mcp_app, flag_issues);
    if (!flag_issues.empty()) {
        if (x.output == "json") print_json({{"status", "error"}, {"error", {{"code", flag_issues.front().code}, {"message", flag_issues.front().message}}}, {"issues", issues_json(flag_issues)}});
        else for (const auto& i : flag_issues) std::cerr << format_issue(i) << "\n";
        return 1;
    }
    std::vector<ConfigIssue> env_issues;
    const Effective effective = resolve(loaded.parsed.config, flags, env, &env_issues);
    if (x.output == "json") {
        json data = {{"config_file", {{"path", label}, {"exists", loaded.exists}}}, {"values", effective_to_json(effective)}};
        json issues = issues_json(loaded.parsed.issues);
        for (const auto& i : issues_json(env_issues)) issues.push_back(i);
        data["issues"] = issues;
        print_json({{"status", "success"}, {"data", data}});
    } else {
        std::cout << "config file: " << label << (loaded.exists ? "" : " (not found, defaults apply)") << "\n\n";
        std::cout << format_effective(effective);
        for (const auto& i : loaded.parsed.issues) std::cout << format_issue(i, label) << "\n";
        for (const auto& i : env_issues) std::cout << format_issue(i) << "\n";
    }
    return 0;
}

// A startup error of the tray started from the Start menu, the autostart or the link tray would only reach a console
// that closes at once (or none): it is shown in a message box as well.
int startup_error(bool tray_mode, const std::string& code, const std::string& message, int exit_code) {
    if (!tray_mode || !tray::startup_error_needs_dialog_here()) return report_error("text", code, message, exit_code);
    auto host = tray::make_win32_tray_host();
    return tray::report_startup_error("fairyfly MCP", code, message, exit_code, true, host.get(),
                                      [](const std::string& line) { std::cerr << line << std::endl; });
}

int run_tray(McpExtras& x, const config::McpConfig& layers, mcp::ServeOptions& options, const LoadedConfig& loaded) {
    (void)layers;
    tray::TrayStart start = tray::prepare_tray_start(tray::kTrayMutexName);
    tray::TrayEntry& entry = start.entry;
    const std::string& exe = entry.exe;
    entry.config_path = x.config_path.empty() && loaded.path.source == Source::Default ? std::string() : loaded.path.path.string();
    if (start.program != exe) entry.autostart_line = tray::autostart_command(start.program, entry.config_path);

    // Autostart install/remove and the relaunch decision. The HTTP requirement is checked first so the
    // user hears about it in the console, not in an invisible child.
    if (!x.install_autostart && !x.remove_autostart && options.transport != "http")
        return startup_error(true, "TRAY_REQUIRES_HTTP",
                             "The tray needs the HTTP transport (--http, or server.transport: http in the config file)", 2);
    entry.install_autostart = x.install_autostart;
    entry.remove_autostart = x.remove_autostart;
    if (auto exit_code = tray::settle_tray_start(start, tray::TrayKind::Mcp, "fairyfly MCP")) return *exit_code;
    // From here on this process is the tray: logs to file, server on the main thread.
    auto& launcher = start.launcher;
    auto& guard = start.guard;
    const std::string log_file = tray::route_logging_to_file();

    auto& factory = tray::runner_factory();
    if (!factory) {
        spdlog::error("TRAY_SERVER_UNAVAILABLE: this build has no HTTP server to run under the tray");
        auto host = tray::make_win32_tray_host();
        host->message_box("fairyfly MCP", "TRAY_SERVER_UNAVAILABLE: this build has no HTTP server to run under the tray.");
        return 2;
    }
    std::unique_ptr<tray::IServerRunner> runner = factory(options);
    if (!runner) return startup_error(true, "TRAY_SERVER_UNAVAILABLE", "The MCP server could not be created; details are in " + log_file, 2);

    auto host = tray::make_win32_tray_host();
    auto opener = tray::make_win32_file_opener();
    tray::TrayServices tray_services;
    tray_services.host = host.get();
    tray_services.opener = opener.get();
    tray_services.launcher = launcher.get();
    auto link_control = link::make_link_control();
    tray_services.link = link_control.get();
    // the update channel (GitHub releases), once in the background; FAIRYFLY_UPDATE_CHECK=0|false|no|off turns it off,
    // an MSIX build never asks (the Store updates it)
    if (packaging::self_updates(packaging::kPackage) && config::Environment::process().enabled(config::env::kUpdateCheck))
        tray_services.latest_update = []() -> std::optional<std::string> {
            auto c = update::check(FAIRYFLY_VERSION);
            return c.available ? std::optional<std::string>(c.latest) : std::nullopt;
        };

    tray::TrayPaths paths;
    paths.log_file = log_file;
    paths.audit_dir = tray_audit_dir(options);
    paths.config_file = loaded.path.path;
    paths.exe = exe;
    tray::TrayState state;
    state.hard_read_only = config::read_only_cap();
    state.self_update = packaging::self_updates(packaging::kPackage);
    if (auto v = layers.get_bool("tray.start_minimized_notice")) state.start_notice = *v;
    return tray::run_with_tray(*runner, tray_services, *guard, std::move(paths), std::move(state));
}

} // namespace

// ---- shared helpers ------------------------------------------------------------------------------
std::filesystem::path tray_audit_dir(const mcp::ServeOptions& options) {
    const auto file = audit::resolve_config(process_env(), false, false, std::chrono::system_clock::now(), options.audit_file).file;
    return std::filesystem::absolute(file).parent_path();   // a relative audit.file is next to the working directory
}

int report_error(const std::string& output, const std::string& code, const std::string& message, int exit_code) {
    if (output == "json") print_json({{"status", "error"}, {"error", {{"code", code}, {"message", message}}}});
    else std::cerr << "error " << code << ": " << message << std::endl;
    return exit_code;
}

LoadedConfig load_config(const std::string& flag_path, const EnvLookup& env) {
    LoadedConfig out;
    out.path = resolve_config_path(flag_path, env);
    std::error_code ec;
    out.exists = std::filesystem::is_regular_file(out.path.path, ec);
    if (!out.exists) return out;
    const auto text = read_text_file(out.path.path);
    if (!text) {
        out.read_error = true;
        return out;
    }
    out.parsed = parse_yaml(*text);
    return out;
}

McpConfig flags_layer(const CLI::App& mcp_app, std::vector<ConfigIssue>& issues) {
    McpConfig layer;
    for (const auto& spec : key_specs()) {  // the key table names the options of each key (first given wins)
        for (const auto& name : spec.cli) {
            const CLI::Option* option = mcp_app.get_option_no_throw(name);
            if (!option || option->count() == 0) continue;
            std::string why;
            if (auto value = parse_value(spec, flag_text(spec, *option), &why)) layer.set(spec.key, std::move(*value));
            else issues.push_back({Severity::Error, "CONFIG_FLAG_INVALID", name + ": " + why, 0, spec.key});
            break;
        }
    }
    if (const CLI::Option* http = mcp_app.get_option_no_throw("--http"); http && http->count() > 0)
        layer.set("server.transport", std::string("http"));
    return layer;
}

void setup_mcp_extras(CLI::App& mcp, McpExtras& x) {
    x.mcp_app = &mcp;
    mcp.add_option("-c,--config", x.config_path, "Config file (default: %LOCALAPPDATA%\\fairyfly\\mcp.yaml; also FAIRYFLY_MCP_CONFIG)");
    mcp.add_flag("--tray", x.tray, "Run in the system tray (detached from the console); needs the HTTP transport");
    mcp.add_flag("--install-autostart", x.install_autostart, "With --tray: start the tray at logon (HKCU Run key)");
    mcp.add_flag("--remove-autostart", x.remove_autostart, "With --tray: remove the logon autostart");

    CLI::App* config = mcp.add_subcommand("config", "Show, locate, create and validate the MCP config file");
    config->fallthrough();
    config->require_subcommand(1);
    x.config_show = config->add_subcommand("show", "Effective settings with the source of each (flag/env/yaml/default)");
    x.config_show->footer(R"HELP(Usage notes:
Example: fairyfly mcp config show
)HELP");
    x.config_path_cmd = config->add_subcommand("path", "Print the config file path");
    x.config_path_cmd->footer(R"HELP(Usage notes:
Example: fairyfly mcp config path
)HELP");
    x.config_init = config->add_subcommand("init", "Write a commented template (never overwrites without --force)");
    x.config_init->footer(R"HELP(Usage notes:
Creates a local config template; inspect it before starting the server.
Example: fairyfly mcp config init
)HELP");
    x.config_init->add_flag("--force", x.init_force, "Overwrite an existing file");
    x.config_validate = config->add_subcommand("validate", "Validate the config file (line-numbered errors)");
    x.config_validate->footer(R"HELP(Usage notes:
Example: fairyfly mcp config validate
)HELP");
    for (CLI::App* leaf : {x.config_show, x.config_path_cmd, x.config_init, x.config_validate}) {
        leaf->fallthrough();
        output_option(leaf, x.output);
    }

    x.client_config = mcp.add_subcommand("client-config", "Print ready-to-paste client configurations (placeholder token only)");
    x.client_config->footer(R"HELP(Usage notes:
Produces configuration instructions; does not connect a client automatically.
Example: fairyfly mcp client-config --stdio --claude-code
)HELP");
    x.client_config->fallthrough();
    x.client_config->add_flag("--claude-code", x.client.claude_code, "Claude Code (claude mcp add and .mcp.json)");
    x.client_config->add_flag("--claude-desktop", x.client.claude_desktop, "Claude Desktop via mcp-remote");
    x.client_config->add_flag("--mcp-remote", x.client.mcp_remote, "mcp-remote command line");
    x.client_config->add_flag("--curl", x.client.curl, "curl smoke test for a Linux host");
    x.client_config->add_flag("--stdio", x.client.stdio, "Local stdio variant (fairyfly mcp)");
    x.client_config->add_option("--url", x.client.url, "Server URL (default https://<host>:" + std::to_string(mcp::kDefaultTlsPort) + "/mcp)");
    x.client_config->add_option("--token-env", x.client.token_env, "NAME of the environment variable that holds the token (default FAIRYFLY_TOKEN)");
    x.client_config->add_option("--name", x.client.name, "Server name in the client (default fairyfly)");
    output_option(x.client_config, x.output);

    x.doctor = mcp.add_subcommand("doctor", "Check the MCP server environment (config, port, SAP GUI, desktop, tokens, http.sys setup: urlacl, sslcert, certificate, TLS, autostart, tray)");
    x.doctor->footer(R"HELP(Usage notes:
Use to diagnose server setup and connectivity prerequisites.
Example: fairyfly mcp doctor
)HELP");
    x.doctor->fallthrough();
    output_option(x.doctor, x.output);
    setup_mcp_setup_commands(mcp, x);
}

std::optional<int> run_mcp_extras(McpExtras& x, mcp::ServeOptions& options, const HandlerProvider& get_handler,
                                  const GlobalOptions& global) {
    if (x.config_path_cmd && *x.config_path_cmd) return cmd_path(x);
    if (x.config_init && *x.config_init) return cmd_init(x);
    if (x.config_validate && *x.config_validate) return cmd_validate(x);
    if (x.config_show && *x.config_show) return cmd_show(x);
    if (x.client_config && *x.client_config) return run_client_config_command(x);
    if (x.doctor && *x.doctor) return run_mcp_doctor_command(x, get_handler);
    if (const auto setup_exit = run_mcp_setup_commands(x, global)) return setup_exit;

    // Server path: apply YAML < env < flags on top of the parsed options.
    const auto env = process_env();
    const LoadedConfig loaded = load_config(x.config_path, env);
    const std::string label = loaded.path.path.string();
    if (loaded.read_error) return startup_error(x.tray, "CONFIG_READ_FAILED", "Cannot read " + label, 2);
    if (loaded.path.explicit_request && !loaded.exists)
        return startup_error(x.tray, "CONFIG_NOT_FOUND", "Config file not found: " + label, 2);
    for (const auto& i : loaded.parsed.issues) {
        if (i.severity == Severity::Error) std::cerr << format_issue(i, label) << std::endl;
        else spdlog::warn("{}", format_issue(i, label));
    }
    if (!loaded.parsed.ok()) return startup_error(x.tray, loaded.parsed.first_error_code(), label + " is invalid; run 'fairyfly mcp config validate'", 2);

    std::vector<ConfigIssue> env_issues;
    const McpConfig env_config = env_layer(env, &env_issues);
    for (const auto& issue : env_issues) {
        if (issue.severity == Severity::Error)
            return startup_error(x.tray, issue.code, issue.message, 2);
        spdlog::warn("{}", format_issue(issue));
    }
    std::vector<ConfigIssue> flag_issues;
    const McpConfig flags = flags_layer(*x.mcp_app, flag_issues);
    if (!flag_issues.empty()) return startup_error(x.tray, flag_issues.front().code, flag_issues.front().message, 2);
    const McpConfig layers = given(resolve_layers(loaded.parsed.config, env_config, flags));  // the env layer read once
    apply_config(layers, options);
    options.audit_file = audit_settings(loaded.parsed.config, env_config, flags);  // audit.required from any layer
    if (!x.tray && layers.get_bool("tray.enabled") != true && !x.install_autostart && !x.remove_autostart) return std::nullopt;
    return run_tray(x, layers, options, loaded);
}

} // namespace commands
} // namespace fairyfly
