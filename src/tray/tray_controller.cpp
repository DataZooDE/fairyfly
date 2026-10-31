#include "include/config/environment.h"
#include "include/text/command_line.h"
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <spdlog/spdlog.h>

#include "include/system/process_probe.h"
#include "include/tray/mcp_tray.h"

namespace fairyfly::tray {

RunnerFactory& runner_factory() {
    static RunnerFactory factory;
    return factory;
}

// ---- Command-line builders (text/command_line.h) -----------------------------------------------
std::string quote_arg(const std::string& arg) { return text::quote_argument<char>(arg); }
std::string join_command_line(const std::string& exe, const std::vector<std::string>& args) {
    return text::join_command_line<char>(exe, args);
}

RelaunchPlan plan_relaunch(const std::string& exe, const std::vector<std::string>& args, bool tray_requested,
                           bool is_child, bool has_console) {
    RelaunchPlan plan;
    if (!tray_requested || is_child || !has_console) return plan;
    plan.relaunch = true;
    plan.command_line = join_command_line(exe, args);
    plan.extra_env.emplace_back(config::env::kTrayChild, "1");
    plan.no_window = true;
    return plan;
}

std::string autostart_command(const std::string& exe, const std::string& config_path) {
    std::string out = "\"" + exe + "\" mcp --tray";
    if (!config_path.empty()) out += " -c \"" + config_path + "\"";
    return out;
}

std::string link_autostart_command(const std::string& exe, const std::vector<std::string>& args) {
    std::vector<std::string> kept;
    bool tray = false;
    for (const auto& a : args) {
        if (a == "--install-autostart" || a == "--remove-autostart") continue;
        tray = tray || a == "--tray";
        kept.push_back(a);
    }
    if (!tray) kept.push_back("--tray");
    return join_command_line(exe, kept);
}

std::string tray_start_program(const std::string& exe, const std::function<bool(const std::filesystem::path&)>& exists) {
    const auto launcher = std::filesystem::path(exe).parent_path() / kTrayLauncherName;
    return exists && exists(launcher) ? launcher.string() : exe;
}

bool settle_autostart(const AutostartPlan& plan, bool launched, RegistryRunKey& run, TraySettings& settings) {
    if (!launched || plan.name.empty() || plan.line.empty()) return false;
    if (const auto current = run.get(plan.name)) {
        const std::string old_program = quote_arg(plan.exe);
        if (plan.program == plan.exe || current->rfind(old_program, 0) != 0) return false;
        return run.set(plan.name, quote_arg(plan.program) + current->substr(old_program.size()));
    }
    if (settings.get(kAutostartDefaultedSetting)) return false;                   // turned off in the menu: stays off
    settings.set(kAutostartDefaultedSetting, "1");
    return run.set(plan.name, plan.line);
}

std::string console_command_line(const std::string& exe, const std::vector<std::string>& args) {
    return "cmd.exe /k \"" + join_command_line(exe, args) + "\"";
}

std::filesystem::path log_directory(const std::function<std::string(const char*)>& env) {
    const auto data = config::Environment(env).data_dir();
    return data.empty() ? std::filesystem::path() : data / "logs";   // no profile: no log file (never a relative one)
}

// ---- Controller ----------------------------------------------------------------------------------
TrayController::TrayController(mcp::IServerControl& control, IServerRunner* runner, TrayServices services,
                               TrayPaths paths, TrayState state)
    : control_(control), runner_(runner), services_(services), paths_(std::move(paths)), state_(std::move(state)) {}

TrayMenuModel TrayController::menu() {
    auto model = build_menu(control_.status(), state_);
    append_sap_startup_menu(model, services_.settings);
    return model;
}

TrayView TrayController::view() {
    const auto model = build_menu(control_.status(), state_);
    return {model.icon, model.tooltip};
}

std::string TrayController::left_click_text() { return status_balloon_text(control_.status()); }

void TrayController::notify(const std::string& text) {
    if (services_.host) services_.host->balloon("fairyfly MCP", text);
}

void TrayController::announce_update(const std::string& version) {
    {
        std::lock_guard<std::mutex> l(update_mu_);
        if (announced_update_ == version) return;
        announced_update_ = version;
    }
    notify("fairyfly " + version + " is available. Install it with: fairyfly update apply");
}

void TrayController::on_tick() {
    if (services_.link) {
        state_.link = services_.link->view();
        if (state_.link.trail_on != last_trail_) {   // the person at the desktop must see when it turns on or off
            notify(trail_balloon_text(state_.link));
            last_trail_ = state_.link.trail_on;
        }
    }
    if (announced_) return;
    const auto status = control_.status();
    if (!status.running && status.warnings.empty()) return;
    announced_ = true;
    if (!status.warnings.empty()) {
        notify(status_balloon_text(status));
    } else if (state_.start_notice) {
        notify("Started: " + (status.endpoint.empty() ? std::string("server") : status.endpoint) +
               (status.read_only ? " (read-only)" : " (write mode)"));
    }
}

void TrayActions::open(const std::filesystem::path& path, const std::string& what) const {
    if ((!opener || !opener->open(path)) && notify) notify("Could not open " + what + ": " + path.string());
}

void TrayActions::console(const std::vector<std::string>& args) const {
    if (!launcher) return;
    LaunchRequest request;
    request.command_line = console_command_line(exe, args);
    request.new_console = true;
    if (!launcher->launch(request) && notify) notify("Could not open a console");
}

bool TrayActions::start_detached(const std::vector<std::string>& args) const {
    if (!launcher) return false;
    LaunchRequest request;
    request.command_line = join_command_line(exe, args);
    request.detached = true;
    return launcher->launch(request).has_value();
}

void TrayController::on_action(TrayAction action) {
    if (action == TrayAction::ToggleSapStartup) {
        if (!toggle_sap_startup(services_.settings, services_.host, "fairyfly MCP")) notify("Could not save the automatic SAP startup setting.");
        return;
    }
    const TrayActions actions{services_.opener, services_.launcher, paths_.exe,
                              [this](const std::string& text) { notify(text); }};
    const auto open = [&](const std::filesystem::path& path, const char* what) { actions.open(path, what); };
    const auto console = [&](const std::vector<std::string>& args) { actions.console(args); };

    switch (action) {
    case TrayAction::Start: control_.request_start(); break;
    case TrayAction::Stop: control_.request_stop(); break;
    case TrayAction::Restart: control_.request_restart(); break;
    case TrayAction::ToggleReadOnly: {
        if (state_.hard_read_only) break;
        const bool currently_read_only = control_.status().read_only;
        if (currently_read_only) {
            // Turning the guard OFF lets clients change SAP data: always ask.
            const bool yes = services_.host &&
                             services_.host->confirm("fairyfly MCP", "Allow the MCP server to change SAP data?");
            if (yes) control_.set_read_only(false);
        } else {
            control_.set_read_only(true);
        }
        break;
    }
    case TrayAction::OpenLog: open(paths_.log_file, "log"); break;
    case TrayAction::OpenAuditFolder: open(paths_.audit_dir, "audit folder"); break;
    case TrayAction::OpenConfig: open(paths_.config_file, "config file"); break;
    case TrayAction::TokenList: console({"mcp", "token", "list"}); break;
    case TrayAction::TokenCreate: console({"mcp", "token", "create"}); break;
    case TrayAction::LinkPair: console({"link", "pair"}); break;
    case TrayAction::UpdateCheck: console({"update", "check"}); break;
    case TrayAction::LinkStatus: console({"link", "status"}); break;
    case TrayAction::LinkStart:   // the link tray: its own icon and Quit; it reads the pairing state
        if (!actions.start_detached({"link", "run", "--tray"})) notify("Could not start the cloud link");
        break;
    case TrayAction::LinkPause:
    case TrayAction::LinkResume: {
        const bool pause = action == TrayAction::LinkPause;
        if (services_.link && services_.link->set_paused(pause)) {
            state_.link = services_.link->view();
            notify(pause ? "Cloud link paused: the AI assistant cannot reach this desktop" : "Cloud link resumed");
        } else {
            notify(pause ? "Could not pause the cloud link" : "Could not resume the cloud link");
        }
        break;
    }
    case TrayAction::Quit:
        control_.request_stop();
        if (runner_) runner_->request_quit();
        if (services_.host) services_.host->post_quit();
        break;
    case TrayAction::LinkToggleWrites:   // the link tray's own switch (link_tray.cpp)
    case TrayAction::StartMcpServer:     // the link tray's item; this IS the MCP server
    case TrayAction::None: break;
    }
    if (services_.host) services_.host->refresh();
}

// ---- run_with_tray -------------------------------------------------------------------------------
std::unique_ptr<UiThread> UiThread::start(TrayHost& host, TrayListener& listener) {
    std::thread ui;
    struct Ready {
        std::mutex m;
        std::condition_variable cv;
        bool signalled = false;
        bool ok = false;
        void set(bool value) {
            std::lock_guard<std::mutex> lock(m);
            if (signalled) return;
            signalled = true;
            ok = value;
            cv.notify_all();
        }
    };
    auto ready = std::make_shared<Ready>();
    ui = std::thread([&host, &listener, ready] {
        try {
            host.run(listener, [ready](bool ok) { ready->set(ok); });
        } catch (...) {
        }
        ready->set(false);   // no-op when run() already reported
    });
    {
        std::unique_lock<std::mutex> lock(ready->m);
        ready->cv.wait(lock, [&] { return ready->signalled; });
    }
    if (ready->ok) return std::unique_ptr<UiThread>(new UiThread(host, std::move(ui)));
    host.post_quit();
    ui.join();
    return nullptr;
}

UiThread::~UiThread() {
    host_.post_quit();
    if (thread_.joinable()) thread_.join();
}

int run_with_tray(IServerRunner& runner, TrayServices services, SingleInstance& guard, TrayPaths paths,
                  TrayState state) {
    if (!services.host) return 1;
    if (!claim_tray(guard, *services.host, services.claim, paths.exe, "fairyfly MCP", "fairyfly MCP tray")) return 0;

    const std::string log_file = paths.log_file.string();
    TrayController controller(runner.control(), &runner, services, std::move(paths), std::move(state));

    // UI thread: window + icon + message pump. The main thread below stays the COM/STA server thread.
    const auto ui = UiThread::start(*services.host, controller);
    if (!ui) {
        services.host->message_box("fairyfly MCP", "The fairyfly MCP tray could not create its tray window. Details are in the log: " +
                                                       log_file);
        return 1;
    }

    // background update check: owns only a shared slot, so a late answer after Quit goes nowhere
    struct UpdateSlot {
        std::mutex m;
        std::condition_variable cv;
        TrayController* controller = nullptr;
        bool quit = false;
    };
    auto slot = std::make_shared<UpdateSlot>();
    slot->controller = &controller;
    if (services.latest_update) {
        std::thread([slot, probe = services.latest_update, delay = services.update_check_delay] {
            {
                std::unique_lock<std::mutex> l(slot->m);
                if (slot->cv.wait_for(l, delay, [&] { return slot->quit; })) return;
            }
            std::optional<std::string> v;
            try { v = probe(); } catch (...) {}
            std::lock_guard<std::mutex> l(slot->m);
            if (v && slot->controller) slot->controller->announce_update(*v);
        }).detach();
    }

    int code = 1;
    try {
        code = runner.run_blocking();
    } catch (...) {
        code = 1;
    }
    {
        std::lock_guard<std::mutex> l(slot->m);
        slot->controller = nullptr;
        slot->quit = true;
    }
    slot->cv.notify_all();
    return code;   // ~UiThread: quit and join the UI
}

// ---- Startup errors ------------------------------------------------------------------------------
bool startup_error_needs_dialog(bool is_tray_child, bool has_console, unsigned console_processes) {
    if (is_tray_child || !has_console) return true;
    return console_processes <= 1;   // only this process: the console window closes when it ends
}

int report_startup_error(const std::string& title, const std::string& code, const std::string& message, int exit_code,
                         bool dialog, TrayHost* host, const std::function<void(const std::string&)>& print) {
    if (print) print("error " + code + ": " + message);
    if (dialog && host) host->message_box(title, code + ": " + message);
    return exit_code;
}

// ---- A held single-instance guard ------------------------------------------------------------------
HeldTray decide_held_tray(const std::vector<RunningTray>& running, const std::string& own_exe,
                          const std::function<bool(const std::filesystem::path&)>& exists, const std::string& tray_name) {
    HeldTray d;
    if (running.empty()) return d;   // no window to tell by: as before
    std::vector<const RunningTray*> removed, other;
    for (const auto& t : running) {
        if (t.image.empty() || system::same_executable(t.image, own_exe)) return d;   // this installation's, or unknown
        // the image is UTF-8 (a user name with umlauts): not the ANSI code page std::string paths use on Windows
        const bool present = exists ? exists(std::filesystem::path(std::u8string(t.image.begin(), t.image.end()))) : true;
        (present ? other : removed).push_back(&t);
    }
    const auto describe = [](const std::vector<const RunningTray*>& trays) {
        std::string out;
        for (const auto* t : trays) out += (out.empty() ? "" : ", ") + std::string("pid ") + std::to_string(t->pid) + ": " + t->image;
        return out;
    };
    if (other.empty()) {
        // an uninstalled MSI left it running from its deleted files: nothing else will ever end it
        d.action = HeldTrayAction::TakeOver;
        for (const auto* t : removed) d.pids.push_back(t->pid);
        d.message = "the running " + tray_name + " belongs to a fairyfly whose files were removed (" + describe(removed) +
                    "); asking it to quit";
        return d;
    }
    d.action = HeldTrayAction::Explain;
    for (const auto* t : other) d.pids.push_back(t->pid);
    d.message = "The " + tray_name + " of another fairyfly installation is already running (" + describe(other) +
                "). Quit it from its icon in the notification area (or uninstall that fairyfly), then start this one again.";
    return d;
}

bool claim_tray(SingleInstance& guard, TrayHost& host, const TrayClaim& claim, const std::string& own_exe, const std::string& title,
                const std::string& tray_name) {
    if (guard.acquire()) return true;
    const HeldTray held = claim.instances ? decide_held_tray(claim.instances->running(), own_exe, claim.exists, tray_name) : HeldTray{};
    // COM started this for a notification (TrayClaim::com_start): nobody asked for a window, so neither the running
    // tray's menu nor a message box about another installation's tray; a removed installation's tray is still taken over
    if (claim.com_start && held.action != HeldTrayAction::TakeOver) {
        spdlog::info("tray: started by COM for a notification while a {} runs; leaving it alone", tray_name);
        return false;
    }
    if (held.action == HeldTrayAction::Explain) {
        spdlog::warn("tray: {}", held.message);
        spdlog::default_logger()->flush();
        host.message_box(title, held.message);
        return false;
    }
    if (held.action == HeldTrayAction::TakeOver) {
        spdlog::warn("tray: {}", held.message);
        for (const auto pid : held.pids) claim.instances->ask_to_quit(pid);
        // another tray of this installation may win the guard first (two started at once): then it is the running one
        const auto won_by_ours = [&] {
            const auto now_running = claim.instances->running();
            return std::any_of(now_running.begin(), now_running.end(),
                               [&](const RunningTray& t) { return !t.image.empty() && system::same_executable(t.image, own_exe); });
        };
        const auto deadline = std::chrono::steady_clock::now() + claim.takeover_wait;
        for (;;) {
            if (guard.acquire()) {
                spdlog::info("tray: the {} of the removed installation has quit; this one runs now", tray_name);
                return true;
            }
            if (won_by_ours()) {
                spdlog::info("tray: another {} of this installation took over first", tray_name);
                if (claim.com_start) return false;
                if (!host.show_running_instance()) host.message_box(title, "The " + tray_name + " is already running.");
                return false;
            }
            if (std::chrono::steady_clock::now() >= deadline) break;
            std::this_thread::sleep_for(claim.poll);
        }
        std::string pids;
        for (const auto pid : held.pids) pids += (pids.empty() ? "" : ", ") + std::to_string(pid);
        const std::string text = "The " + tray_name + " of a fairyfly whose files were removed is still running and did not quit "
                                 "when asked (fairyfly.exe, PID " + pids + "). End it in Task Manager (Details), then start "
                                 "fairyfly again.";
        spdlog::warn("tray: {}", text);
        spdlog::default_logger()->flush();
        host.message_box(title, text);
        return false;
    }
    if (!host.show_running_instance()) host.message_box(title, "The " + tray_name + " is already running.");
    return false;
}

// ---- Entry decision ------------------------------------------------------------------------------
TrayEntryResult decide_tray_entry(const TrayEntry& entry, const TrayEntryServices& services) {
    TrayEntryResult result;
    auto say = [&](const std::string& text) {
        result.message = text;
        if (services.print) services.print(text);
    };
    auto fail = [&](const std::string& text) {
        result.outcome = TrayEntryOutcome::Failed;
        result.exit_code = 1;
        say(text);
        return result;
    };

    if (entry.install_autostart && entry.remove_autostart)
        return fail("--install-autostart and --remove-autostart cannot be combined");

    if ((entry.install_autostart || entry.remove_autostart) && !entry.run_key_autostart)
        return fail("TRAY_AUTOSTART_PACKAGED: this fairyfly is a Microsoft Store package; it starts the link tray at logon "
                    "itself (switch it in Settings > Apps > Startup) and writes no Run value");

    if (entry.install_autostart) {
        if (!services.registry) return fail("TRAY_AUTOSTART_FAILED: no registry access");
        const std::string command = entry.autostart_line.empty() ? autostart_command(entry.exe, entry.config_path) : entry.autostart_line;
        if (!services.registry->set(entry.autostart_name, command)) return fail("TRAY_AUTOSTART_FAILED: could not write the Run key");
        result.outcome = TrayEntryOutcome::AutostartDone;
        say("Autostart registered (HKCU Run \\ " + entry.autostart_name + "): " + command +
            "\nIt only starts while this user is logged on to an interactive desktop (see docs/MCP_TRAY.md).");
        return result;
    }
    if (entry.remove_autostart) {
        if (!services.registry) return fail("TRAY_AUTOSTART_FAILED: no registry access");
        if (!services.registry->remove(entry.autostart_name)) return fail("TRAY_AUTOSTART_FAILED: could not remove the Run value");
        result.outcome = TrayEntryOutcome::AutostartDone;
        say("Autostart removed.");
        return result;
    }

    if (!entry.tray_requested) return result;   // NotTray

    if (entry.is_child) {
        result.outcome = TrayEntryOutcome::RunHere;
        return result;
    }
    if (services.guard && services.guard->exists()) {
        result.outcome = TrayEntryOutcome::AlreadyRunning;
        say(entry.name + " is already running.");
        return result;
    }
    const RelaunchPlan plan = plan_relaunch(entry.exe, entry.args, entry.tray_requested, entry.is_child, entry.has_console);
    if (!plan.relaunch) {
        result.outcome = TrayEntryOutcome::RunHere;   // no console to hand back: this process becomes the tray
        return result;
    }
    if (!services.launcher) return fail("TRAY_RELAUNCH_FAILED: no process launcher");
    LaunchRequest request;
    request.command_line = plan.command_line;
    request.detached = plan.no_window;
    request.extra_env = plan.extra_env;
    const auto pid = services.launcher->launch(request);
    if (!pid) return fail("TRAY_RELAUNCH_FAILED: could not start the detached tray process");
    result.outcome = TrayEntryOutcome::Relaunched;
    say(entry.name + " started (pid " + std::to_string(*pid) + ").");
    return result;
}

} // namespace fairyfly::tray
