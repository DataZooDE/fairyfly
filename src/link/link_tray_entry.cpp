// `fairyfly link run --tray`: the cloud link alone in the system tray (no MCP server, no HTTP).
#include <chrono>
#include <iostream>

#include <spdlog/spdlog.h>

#include "include/audit_log.h"
#include "include/config/mcp_config.h"
#include "include/link/link_console.h"
#include "include/link/link_runner.h"
#include "include/packaging.h"
#include "include/tray/tray.h"
#include "include/tray/tray_win32.h"

namespace fairyfly::link {

int run_link_tray(const LinkOptions& o, const std::function<cli::CommandHandler&()>& get_handler, const commands::GlobalOptions& global,
                  audit::AuditSink* sink, const std::function<cli::CommandHandler*()>& peek) {
    // before anything else: autostart and the tray's Pause act on the pairing state and the shared Run value
    if (auto refused = link_state_guard(o)) {
        std::cerr << refused->to_json().dump() << std::endl;
        if (tray::startup_error_needs_dialog_here())   // started from the Start menu or the autostart: nobody reads stderr
            tray::make_win32_tray_host()->message_box("fairyfly link", refused->error.value("message", std::string("the pairing state was refused")));
        return 2;
    }
    tray::TrayStart start = tray::prepare_tray_start(tray::kLinkTrayMutexName);
    tray::TrayEntry& entry = start.entry;
    const std::string& exe = entry.exe;
    const std::string& program = start.program;
    entry.install_autostart = o.install_autostart;
    entry.remove_autostart = o.remove_autostart;
    entry.autostart_name = tray::kLinkAutostartValueName;
    entry.autostart_line = tray::link_autostart_command(program, entry.args);
    entry.name = "fairyfly link tray";

    // An unpaired desktop still gets the tray: it leads to "Pair with the fairyfly cloud" (run_link_with_tray). The
    // Start-menu entry runs this command, so refusing here would end in a console that closes at once.
    auto link_control = make_link_control(o.state_path);

    if (auto exit_code = tray::settle_tray_start(start, tray::TrayKind::Link, "fairyfly link")) return *exit_code;
    // From here on this process is the link tray: logs to link.log, the link on the main thread.
    const bool launched = start.launched;   // the Start menu, the autostart or the installer
    auto& registry = start.registry;
    auto& launcher = start.launcher;
    auto& guard = start.guard;
    const std::string log_file = tray::route_logging_to_file("link.log");

    // "Start with Windows": on after the first start from the Start menu, the menu switches it
    tray::AutostartPlan autostart;
    autostart.name = tray::kLinkAutostartValueName;
    autostart.line = entry.autostart_line;
    autostart.exe = exe;
    autostart.program = program;
    // an MSIX package starts the tray through its StartupTask: no Run value, no "Start with Windows" item
    auto settings = tray::make_win32_tray_settings();
    if (entry.run_key_autostart && tray::settle_autostart(autostart, launched, *registry, *settings))
        spdlog::info("link tray: Start with Windows set");

    auto host = tray::make_win32_tray_host(tray::TrayKind::Link);
    auto opener = tray::make_win32_file_opener();
    tray::LinkTrayServices tray_services;
    tray_services.host = host.get();
    tray_services.opener = opener.get();
    tray_services.launcher = launcher.get();
    tray_services.link = link_control.get();
    auto write_mode = make_write_mode_control(o.read_only);
    tray_services.writes = write_mode.get();
    auto mcp_tray = tray::make_win32_single_instance(tray::kTrayMutexName);   // probed only: "Start local MCP server"
    tray_services.mcp_tray = mcp_tray.get();
    tray_services.stop_link = [] { request_stop(); };
    if (entry.run_key_autostart) tray_services.run_key = registry.get();
    tray_services.self_update = packaging::self_updates(packaging::kPackage);
    tray_services.autostart = autostart;
    // a tray left running by a removed installation (an uninstalled MSI) is asked to quit, another installation's
    // (MSI beside the Microsoft Store package) is named instead of this start ending without a word
    auto instances = tray::make_win32_tray_instances(tray::TrayKind::Link);
    tray_services.claim.instances = instances.get();
    tray_services.claim.exists = tray::file_not_missing;
    tray_services.claim.com_start = start.com_start;   // -Embedding: a running link tray keeps its menu closed
    tray_services.link_busy = [lock = link_run_lock_name(o.state_path.empty() ? default_state_path() : o.state_path)] {
        return tray::make_win32_single_instance(lock)->exists();
    };

    tray::TrayPaths paths;
    paths.log_file = log_file;
    paths.audit_dir = audit::resolve_config(config::process_env(), false, false, std::chrono::system_clock::now()).file.parent_path();
    paths.exe = exe;

    LinkOptions run_options = o;
    run_options.tray = true;
    return tray::run_link_with_tray([&] { return run_link(run_options, get_handler, global, sink, peek); }, tray_services, *guard,
                                    std::move(paths));
}

}  // namespace fairyfly::link
