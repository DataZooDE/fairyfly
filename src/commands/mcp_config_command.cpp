#include <chrono>
#include <iostream>

#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include "include/audit_log.h"
#include "include/commands/mcp_extras.h"
#include "include/tray/tray.h"
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

/// CLI option names that feed each config key (first match with a count wins).
struct FlagMap { const char* key; std::vector<const char*> options; };
const std::vector<FlagMap>& flag_map() {
    static const std::vector<FlagMap> map = {
        {"server.host", {"--mcp-host"}},
        {"server.port", {"--mcp-port", "--port"}},
        {"server.transport", {"--transport"}},
        {"server.tls", {"--tls"}},
        {"server.allow_ip", {"--allow-ip"}},
        {"server.sse", {"--sse"}},
        {"server.allowed_hosts", {"--allowed-hosts"}},
        {"server.cors_origins", {"--cors-origin"}},
        {"mode.read_only", {"--read-only"}},
        {"mode.allow_write", {"--allow-write"}},
        {"limits.max_result_chars", {"--max-result-chars"}},
        {"limits.max_image_bytes", {"--max-image-bytes"}},
        {"limits.max_calls_per_minute", {"--max-calls-per-minute"}},
        {"limits.call_timeout_ms", {"--call-timeout-ms"}},
        {"tools.families", {"--tools"}},
        {"default_connection", {"--default-connection"}},
        {"format", {"--format"}},
        {"tray.enabled", {"--tray"}},
    };
    return map;
}

std::string join_results(const CLI::Option& option) {
    std::string out;
    for (const auto& r : option.results()) out += (out.empty() ? "" : ",") + r;
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
    std::vector<ConfigIssue> env_issues;
    const Effective effective = resolve(loaded.parsed.config, flags_layer(*x.mcp_app), env, &env_issues);
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

std::string env_or_empty(const char* name) { return process_env()(name); }

bool truthy(const std::string& v) {
    return v == "1" || v == "true" || v == "TRUE" || v == "yes" || v == "on";
}

int run_tray(McpExtras& x, const config::McpConfig& layers, mcp::ServeOptions& options, const LoadedConfig& loaded) {
    (void)layers;
    const std::string exe = tray::current_exe_path();
    tray::TrayEntry entry;
    entry.tray_requested = true;
    entry.is_child = tray::is_tray_child_process();
    entry.has_console = tray::process_has_console();
    entry.exe = exe;
    entry.config_path = x.config_path.empty() && loaded.path.source == Source::Default ? std::string() : loaded.path.path.string();
    entry.args = tray::current_args();   // the detached child gets exactly the same arguments

    auto registry = tray::make_win32_run_key();
    auto launcher = tray::make_win32_process_launcher();
    auto guard = tray::make_win32_single_instance();
    tray::TrayEntryServices services;
    services.registry = registry.get();
    services.launcher = launcher.get();
    services.guard = guard.get();
    services.print = [](const std::string& line) { std::cout << line << std::endl; };

    // Autostart install/remove and the relaunch decision. The HTTP requirement is checked first so the
    // user hears about it in the console, not in an invisible child.
    if (!x.install_autostart && !x.remove_autostart && options.transport != "http")
        return report_error("text", "TRAY_REQUIRES_HTTP",
                            "The tray needs the HTTP transport (--http, or server.transport: http in the config file)", 2);
    entry.install_autostart = x.install_autostart;
    entry.remove_autostart = x.remove_autostart;
    const tray::TrayEntryResult decided = tray::decide_tray_entry(entry, services);
    if (decided.outcome != tray::TrayEntryOutcome::RunHere) return decided.exit_code;

    // From here on this process is the tray: no console, logs to file, server on the main thread.
    if (entry.is_child) tray::detach_console();
    const std::string log_file = tray::route_logging_to_file();

    auto& factory = tray::runner_factory();
    if (!factory) {
        spdlog::error("TRAY_SERVER_UNAVAILABLE: this build has no HTTP server to run under the tray");
        auto host = tray::make_win32_tray_host();
        host->message_box("fairyfly MCP", "TRAY_SERVER_UNAVAILABLE: this build has no HTTP server to run under the tray.");
        return 2;
    }
    std::unique_ptr<tray::IServerRunner> runner = factory(options);
    if (!runner) return 2;

    auto host = tray::make_win32_tray_host();
    auto opener = tray::make_win32_file_opener();
    tray::TrayServices tray_services;
    tray_services.host = host.get();
    tray_services.opener = opener.get();
    tray_services.launcher = launcher.get();

    tray::TrayPaths paths;
    paths.log_file = log_file;
    paths.audit_dir = audit::resolve_config(process_env(), false, false, std::chrono::system_clock::now()).file.parent_path();
    paths.config_file = loaded.path.path;
    paths.exe = exe;
    tray::TrayState state;
    state.hard_read_only = truthy(env_or_empty("FAIRYFLY_READ_ONLY"));
    if (auto v = layers.get_bool("tray.start_minimized_notice")) state.start_notice = *v;
    return tray::run_with_tray(*runner, tray_services, *guard, std::move(paths), std::move(state));
}

} // namespace

// ---- shared helpers ------------------------------------------------------------------------------
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

McpConfig flags_layer(const CLI::App& mcp_app) {
    McpConfig layer;
    for (const auto& entry : flag_map()) {
        const KeySpec* spec = find_key(entry.key);
        if (!spec) continue;
        for (const char* name : entry.options) {
            const CLI::Option* option = mcp_app.get_option_no_throw(name);
            if (!option || option->count() == 0) continue;
            if (auto value = parse_value(*spec, join_results(*option))) layer.set(entry.key, std::move(*value));
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
    x.config_path_cmd = config->add_subcommand("path", "Print the config file path");
    x.config_init = config->add_subcommand("init", "Write a commented template (never overwrites without --force)");
    x.config_init->add_flag("--force", x.init_force, "Overwrite an existing file");
    x.config_validate = config->add_subcommand("validate", "Validate the config file (line-numbered errors)");
    for (CLI::App* leaf : {x.config_show, x.config_path_cmd, x.config_init, x.config_validate}) {
        leaf->fallthrough();
        output_option(leaf, x.output);
    }

    x.client_config = mcp.add_subcommand("client-config", "Print ready-to-paste client configurations (placeholder token only)");
    x.client_config->fallthrough();
    x.client_config->add_flag("--claude-code", x.client.claude_code, "Claude Code (claude mcp add and .mcp.json)");
    x.client_config->add_flag("--claude-desktop", x.client.claude_desktop, "Claude Desktop via mcp-remote");
    x.client_config->add_flag("--mcp-remote", x.client.mcp_remote, "mcp-remote command line");
    x.client_config->add_flag("--curl", x.client.curl, "curl smoke test for a Linux host");
    x.client_config->add_flag("--stdio", x.client.stdio, "Local stdio variant (fairyfly mcp)");
    x.client_config->add_option("--url", x.client.url, "Server URL (default https://<host>:8443/mcp)");
    x.client_config->add_option("--token-env", x.client.token_env, "NAME of the environment variable that holds the token (default FAIRYFLY_TOKEN)");
    x.client_config->add_option("--name", x.client.name, "Server name in the client (default fairyfly)");
    output_option(x.client_config, x.output);

    x.doctor = mcp.add_subcommand("doctor", "Check the MCP server environment (config, port, SAP GUI, desktop, tokens, http.sys setup: urlacl, sslcert, certificate, TLS, autostart, tray)");
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
    if (loaded.read_error) return report_error("text", "CONFIG_READ_FAILED", "Cannot read " + label, 2);
    if (loaded.path.explicit_request && !loaded.exists)
        return report_error("text", "CONFIG_NOT_FOUND", "Config file not found: " + label, 2);
    for (const auto& i : loaded.parsed.issues) {
        if (i.severity == Severity::Error) std::cerr << format_issue(i, label) << std::endl;
        else spdlog::warn("{}", format_issue(i, label));
    }
    if (!loaded.parsed.ok()) return report_error("text", loaded.parsed.first_error_code(), label + " is invalid; run 'fairyfly mcp config validate'", 2);

    const McpConfig layers = merged_layers(loaded.parsed.config, flags_layer(*x.mcp_app), env);
    apply_config(layers, options);
    if (!x.tray && layers.get_bool("tray.enabled") != true && !x.install_autostart && !x.remove_autostart) return std::nullopt;
    return run_tray(x, layers, options, loaded);
}

} // namespace commands
} // namespace fairyfly
