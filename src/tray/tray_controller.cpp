#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

#include "include/tray/tray.h"

namespace fairyfly::tray {

RunnerFactory& runner_factory() {
    static RunnerFactory factory;
    return factory;
}

// ---- Command-line builders ---------------------------------------------------------------------
std::string quote_arg(const std::string& arg) {
    if (!arg.empty() && arg.find_first_of(" \t\n\v\"") == std::string::npos) return arg;
    std::string out = "\"";
    size_t backslashes = 0;
    for (char c : arg) {
        if (c == '\\') {
            ++backslashes;
        } else if (c == '"') {
            out.append(backslashes * 2 + 1, '\\');
            out += '"';
            backslashes = 0;
        } else {
            out.append(backslashes, '\\');
            out += c;
            backslashes = 0;
        }
    }
    out.append(backslashes * 2, '\\');
    out += '"';
    return out;
}

std::string join_command_line(const std::string& exe, const std::vector<std::string>& args) {
    std::string out = quote_arg(exe);
    for (const auto& a : args) out += " " + quote_arg(a);
    return out;
}

RelaunchPlan plan_relaunch(const std::string& exe, const std::vector<std::string>& args, bool tray_requested,
                           bool is_child, bool has_console) {
    RelaunchPlan plan;
    if (!tray_requested || is_child || !has_console) return plan;
    plan.relaunch = true;
    plan.command_line = join_command_line(exe, args);
    plan.extra_env.emplace_back(kTrayChildEnv, "1");
    plan.no_window = true;
    return plan;
}

std::string autostart_command(const std::string& exe, const std::string& config_path) {
    std::string out = "\"" + exe + "\" mcp --tray";
    if (!config_path.empty()) out += " -c \"" + config_path + "\"";
    return out;
}

std::string console_command_line(const std::string& exe, const std::vector<std::string>& args) {
    return "cmd.exe /k \"" + join_command_line(exe, args) + "\"";
}

std::filesystem::path log_directory(const std::function<std::string(const char*)>& env) {
    const std::string base = env ? env("LOCALAPPDATA") : std::string();
    std::filesystem::path root;
    if (!base.empty()) root = base;
    else {
        std::error_code ec;
        root = std::filesystem::temp_directory_path(ec);
    }
    return root / "fairyfly" / "logs";
}

// ---- Controller ----------------------------------------------------------------------------------
TrayController::TrayController(mcp::IServerControl& control, IServerRunner* runner, TrayServices services,
                               TrayPaths paths, TrayState state)
    : control_(control), runner_(runner), services_(services), paths_(std::move(paths)), state_(std::move(state)) {}

TrayMenuModel TrayController::menu() { return build_menu(control_.status(), state_); }

TrayView TrayController::view() {
    const auto model = build_menu(control_.status(), state_);
    return {model.icon, model.tooltip};
}

std::string TrayController::left_click_text() { return status_balloon_text(control_.status()); }

void TrayController::notify(const std::string& text) {
    if (services_.host) services_.host->balloon("fairyfly MCP", text);
}

void TrayController::on_tick() {
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

void TrayController::on_action(TrayAction action) {
    auto open = [&](const std::filesystem::path& path, const char* what) {
        if (!services_.opener || !services_.opener->open(path)) notify(std::string("Could not open ") + what + ": " + path.string());
    };
    auto console = [&](const std::vector<std::string>& args) {
        if (!services_.launcher) return;
        LaunchRequest request;
        request.command_line = console_command_line(paths_.exe, args);
        request.new_console = true;
        if (!services_.launcher->launch(request)) notify("Could not open a console");
    };

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
    case TrayAction::Quit:
        control_.request_stop();
        if (runner_) runner_->request_quit();
        if (services_.host) services_.host->post_quit();
        break;
    case TrayAction::None: break;
    }
    if (services_.host) services_.host->refresh();
}

// ---- run_with_tray -------------------------------------------------------------------------------
int run_with_tray(IServerRunner& runner, TrayServices services, SingleInstance& guard, TrayPaths paths,
                  TrayState state) {
    if (!services.host) return 1;
    if (!guard.acquire()) {
        services.host->message_box("fairyfly MCP", "The fairyfly MCP tray is already running.");
        return 0;
    }

    TrayController controller(runner.control(), &runner, services, std::move(paths), std::move(state));

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
    } ready;

    // UI thread: window + icon + message pump. The main thread below stays the COM/STA server thread.
    std::thread ui([&] {
        try {
            services.host->run(controller, [&](bool ok) { ready.set(ok); });
        } catch (...) {
        }
        ready.set(false);   // no-op when run() already reported
    });

    {
        std::unique_lock<std::mutex> lock(ready.m);
        ready.cv.wait(lock, [&] { return ready.signalled; });
    }
    if (!ready.ok) {
        services.host->post_quit();
        ui.join();
        return 1;
    }

    int code = 1;
    try {
        code = runner.run_blocking();
    } catch (...) {
        code = 1;
    }
    services.host->post_quit();
    ui.join();
    return code;
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

    if (entry.install_autostart) {
        if (!services.registry) return fail("TRAY_AUTOSTART_FAILED: no registry access");
        const std::string command = autostart_command(entry.exe, entry.config_path);
        if (!services.registry->set(kAutostartValueName, command)) return fail("TRAY_AUTOSTART_FAILED: could not write the Run key");
        result.outcome = TrayEntryOutcome::AutostartDone;
        say("Autostart registered (HKCU Run \\ " + std::string(kAutostartValueName) + "): " + command +
            "\nIt only starts while this user is logged on to an interactive desktop (see docs/MCP_TRAY.md).");
        return result;
    }
    if (entry.remove_autostart) {
        if (!services.registry) return fail("TRAY_AUTOSTART_FAILED: no registry access");
        if (!services.registry->remove(kAutostartValueName)) return fail("TRAY_AUTOSTART_FAILED: could not remove the Run value");
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
        say("fairyfly MCP tray is already running.");
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
    say("fairyfly MCP tray started (pid " + std::to_string(*pid) + ").");
    return result;
}

} // namespace fairyfly::tray
