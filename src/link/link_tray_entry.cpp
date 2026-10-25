// `fairyfly link run --tray`: the cloud link alone in the system tray (no MCP server, no HTTP).
#include <chrono>
#include <iostream>

#include <spdlog/spdlog.h>

#include "include/audit_log.h"
#include "include/config/mcp_config.h"
#include "include/link/link_console.h"
#include "include/link/link_runner.h"
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
    const std::string exe = tray::current_exe_path();
    tray::TrayEntry entry;
    entry.tray_requested = true;
    entry.is_child = tray::is_tray_child_process();
    entry.has_console = tray::process_has_console();
    entry.install_autostart = o.install_autostart;
    entry.remove_autostart = o.remove_autostart;
    entry.exe = exe;
    entry.args = tray::current_args();   // the detached child (and the autostart) get the same options
    entry.autostart_name = tray::kLinkAutostartValueName;
    // the Run value starts fairyfly-tray.exe when it is installed: no console window at logon
    const std::string program = tray::tray_start_program(exe, tray::path_exists);
    entry.autostart_line = tray::link_autostart_command(program, entry.args);
    entry.name = "fairyfly link tray";

    // An unpaired desktop still gets the tray: it leads to "Pair with the fairyfly cloud" (run_link_with_tray). The
    // Start-menu entry runs this command, so refusing here would end in a console that closes at once.
    auto link_control = make_link_control(o.state_path);

    auto registry = tray::make_win32_run_key();
    auto launcher = tray::make_win32_process_launcher();
    auto guard = tray::make_win32_single_instance(tray::kLinkTrayMutexName);
    tray::TrayEntryServices services;
    services.registry = registry.get();
    services.launcher = launcher.get();
    services.guard = guard.get();
    services.print = [](const std::string& line) { std::cout << line << std::endl; };
    const tray::TrayEntryResult decided = tray::decide_tray_entry(entry, services);
    if (decided.outcome == tray::TrayEntryOutcome::Failed && tray::startup_error_needs_dialog_here())
        tray::make_win32_tray_host()->message_box("fairyfly link", decided.message);   // nobody reads this console
    if (decided.outcome == tray::TrayEntryOutcome::AlreadyRunning) {
        auto other = tray::make_win32_tray_host(tray::TrayKind::Link);
        if (!other->show_running_instance() && tray::startup_error_needs_dialog_here())
            other->message_box("fairyfly link", "The fairyfly link tray is already running: its icon is in the notification area.");
    }
    if (decided.outcome != tray::TrayEntryOutcome::RunHere) return decided.exit_code;

    // From here on this process is the link tray: no console, logs to link.log, the link on the main thread.
    if (entry.is_child) tray::detach_console();
    const bool launched = tray::is_tray_launched_process();   // the Start menu, the autostart or the installer
    tray::clear_tray_child_marker();   // consoles and processes started from the tray are not tray children
    const std::string log_file = tray::route_logging_to_file("link.log");

    // "Start with Windows": on after the first start from the Start menu, the menu switches it
    tray::AutostartPlan autostart;
    autostart.name = tray::kLinkAutostartValueName;
    autostart.line = entry.autostart_line;
    autostart.exe = exe;
    autostart.program = program;
    auto settings = tray::make_win32_tray_settings();
    if (tray::settle_autostart(autostart, launched, *registry, *settings)) spdlog::info("link tray: Start with Windows set");

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
    tray_services.run_key = registry.get();
    tray_services.autostart = autostart;
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
