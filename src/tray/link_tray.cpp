// The link tray (`fairyfly link run --tray`): the cloud link on its own, without the MCP server.
#include "include/link/link_state_names.h"
#include <thread>

#include <spdlog/spdlog.h>

#include "include/tray/tray.h"

namespace fairyfly::tray {

namespace {

std::string who(const LinkView& link) { return link.organisation + " (" + link.account + ")"; }

} // namespace

std::vector<std::string> mcp_tray_args() { return {"mcp", "--tray", "--http"}; }

TrayMenuModel build_link_menu(const LinkView& link, const LinkWritesView& writes, const McpTrayView& mcp,
                              const AutostartView& autostart, bool self_update) {
    TrayMenuModel model;
    if (link.state == link::state::kConnected) model.icon = IconState::Running;
    else if (link.state == link::state::kPaused) model.icon = IconState::Warning;
    else model.icon = IconState::Stopped;
    const std::string clock = link.state == "connected" ? clock_skew_text(link) : std::string();
    if (!clock.empty()) model.icon = IconState::Warning;   // connected, but every call is refused until the clock is synced
    // connected, but SAP GUI does not answer a call, and every later one waits behind it
    const std::string stuck = link.state == link::state::kConnected ? stuck_call_text(link) : std::string();
    if (!stuck.empty()) model.icon = IconState::Warning;

    if (link.paired) {
        model.items.push_back(item(who(link) + ": " + link.state, TrayAction::None, false));
        if (!clock.empty()) model.items.push_back(item(clock, TrayAction::None, false));
        if (!stuck.empty()) model.items.push_back(item(stuck, TrayAction::None, false));
        if (link.trail_on) model.items.push_back(item(trail_menu_text(link), TrayAction::None, false));
        model.items.push_back(separator());
        if (link.paused) model.items.push_back(item("Resume", TrayAction::LinkResume));
        else model.items.push_back(item("Pause", TrayAction::LinkPause));
        if (writes.available) {
            // the standing approval: on, changes run without asking; off, every change asks at this desktop
            const std::string label = writes.locked     ? "Allow changes in SAP (off: FAIRYFLY_READ_ONLY / --read-only)"
                                      : writes.enforced ? "Allow changes in SAP (off: Approval is required by your administrator)"
                                                        : "Allow changes in SAP";
            MenuItem allow = item(label, TrayAction::LinkToggleWrites, !writes.locked && !writes.enforced);
            allow.checkable = true;
            allow.checked = writes.allowed && !writes.locked && !writes.enforced;
            model.items.push_back(std::move(allow));
        }
        model.items.push_back(item("Health check (console)", TrayAction::LinkStatus));
        model.items.push_back(item("Pair again (console)", TrayAction::LinkPair));
    } else {
        model.items.push_back(item("Not paired with the fairyfly cloud yet", TrayAction::None, false));
        model.items.push_back(separator());
        model.items.push_back(item("Pair with the fairyfly cloud... (console)", TrayAction::LinkPair));
    }
    if (mcp.available) {
        model.items.push_back(separator());
        MenuItem start = item(mcp.running ? "Local MCP server running" : "Start local MCP server", TrayAction::StartMcpServer,
                              !mcp.running);
        start.checkable = true;
        start.checked = mcp.running;
        model.items.push_back(std::move(start));
    }
    if (autostart.available) {
        model.items.push_back(separator());
        MenuItem start = item("Start with Windows", TrayAction::ToggleAutostart);
        start.checkable = true;
        start.checked = autostart.on;
        model.items.push_back(std::move(start));
    }
    model.items.push_back(separator());
    model.items.push_back(item("Open log", TrayAction::OpenLog));
    model.items.push_back(item("Open audit trail folder", TrayAction::OpenAuditFolder));
    model.items.push_back(version_item());
    if (self_update) model.items.push_back(item("Check for updates (console)", TrayAction::UpdateCheck));
    model.items.push_back(separator());
    model.items.push_back(item("Quit", TrayAction::Quit));

    std::string tooltip = "fairyfly link: " + (link.paired ? link.state : std::string("not paired - click to pair"));
    if (writes.available)
        tooltip += writes.locked                                ? " (read-only)"
                   : writes.allowed && !writes.enforced ? " (changes allowed)"
                                                        : " (changes need your approval)";
    if (!clock.empty()) {   // before the names: the tooltip is cut at 127 bytes
        const auto d = link.clock_skew_s < 0 ? -link.clock_skew_s : link.clock_skew_s;
        tooltip += " - clock " + std::to_string(d) + " s " + (link.clock_skew_s < 0 ? "ahead" : "behind") + ", sync it";
    }
    if (!stuck.empty())   // before the names: the tooltip is cut at 127 bytes
        tooltip += " - SAP GUI not answering " + link.stuck_tool + " (" + std::to_string(link.stuck_s) + " s)";
    if (link.trail_on) tooltip += " - audit trail on";   // before the names: the tooltip is cut at 127 bytes
    if (link.paired) tooltip += " - " + who(link);
    model.tooltip = fit_tooltip(tooltip);
    return model;
}

LinkTrayController::LinkTrayController(LinkTrayServices services, TrayPaths paths)
    : services_(std::move(services)), paths_(std::move(paths)) {}

LinkWritesView LinkTrayController::writes_view() {
    LinkWritesView v;
    if (!services_.writes) return v;
    v.available = true;
    v.allowed = services_.writes->allowed();
    v.locked = services_.writes->locked();
    v.enforced = services_.writes->enforced();
    return v;
}

McpTrayView LinkTrayController::mcp_view() {
    McpTrayView v;
    v.available = services_.launcher != nullptr;
    v.running = services_.mcp_tray && services_.mcp_tray->exists();
    return v;
}

AutostartView LinkTrayController::autostart_view() {
    AutostartView v;
    if (!services_.run_key || services_.autostart.name.empty()) return v;
    v.available = true;
    v.on = services_.run_key->get(services_.autostart.name).has_value();
    return v;
}

TrayMenuModel LinkTrayController::menu() {
    auto model = build_link_menu(link_, writes_view(), mcp_view(), autostart_view(), services_.self_update);
    append_sap_startup_menu(model, services_.settings);
    return model;
}

TrayView LinkTrayController::view() {
    const auto model = build_link_menu(link_, writes_view());
    return {model.icon, model.tooltip};
}

std::string LinkTrayController::left_click_text() { return build_link_menu(link_, writes_view()).tooltip; }

void LinkTrayController::notify(const std::string& text) {
    if (services_.host) services_.host->balloon("fairyfly link", text);
}

void LinkTrayController::on_tick() {
    if (!services_.link) return;
    link_ = services_.link->view();
    if (link_.trail_on != last_trail_) {
        notify(trail_balloon_text(link_));
        last_trail_ = link_.trail_on;
    }
    if (link_.clock_skew_s != 0 && last_clock_skew_ == 0) notify(clock_skew_text(link_));   // once, when it starts
    last_clock_skew_ = link_.clock_skew_s;
    if (!link_.stuck_tool.empty() && last_stuck_tool_.empty()) notify(stuck_call_text(link_));   // once, when it starts
    last_stuck_tool_ = link_.stuck_tool;
    const std::string state = link_.state;
    if (state == last_state_) return;
    // "not running" before the first status is written (start) or after Quit says nothing new
    if (state == link::state::kConnected) notify(last_state_ == link::state::kPaused ? "Cloud link resumed" : "Cloud link running for " + who(link_));
    else if (state == link::state::kPaused) notify("Cloud link paused: the AI assistant cannot reach this desktop");
    else if (!last_state_.empty() && !quit_requested_) notify("Cloud link stopped");
    last_state_ = state;
}

void LinkTrayController::on_action(TrayAction action) {
    if (action == TrayAction::ToggleSapStartup) {
        if (!toggle_sap_startup(services_.settings)) notify("Could not save the automatic SAP startup setting.");
        return;
    }
    const TrayActions actions{services_.opener, services_.launcher, paths_.exe,
                              [this](const std::string& text) { notify(text); }};
    const auto open = [&](const std::filesystem::path& path, const char* what) { actions.open(path, what); };
    const auto console = [&](const std::vector<std::string>& args) { actions.console(args); };

    switch (action) {
    case TrayAction::LinkPause:
    case TrayAction::LinkResume: {
        const bool pause = action == TrayAction::LinkPause;
        if (services_.link && services_.link->set_paused(pause)) link_.paused = pause;   // the notice follows on the tick
        else notify(pause ? "Could not pause the cloud link" : "Could not resume the cloud link");
        break;
    }
    case TrayAction::LinkToggleWrites: {
        if (!services_.writes || services_.writes->locked() || services_.writes->enforced()) break;
        const bool allow = !services_.writes->allowed();
        if (allow && services_.host &&
            !services_.host->confirm("fairyfly link",
                                     "Let AI assistants change data in SAP through the cloud link without asking at this "
                                     "desktop?\n\nChanges still need your organisation's grant. Turn this off again to "
                                     "approve every change yourself."))
            break;
        if (!services_.writes->set_allowed(allow)) notify("Could not change \"Allow changes in SAP\"");
        else notify(allow ? "Changes in SAP allowed: they run without asking"
                          : "Every change in SAP asks for your approval at this desktop");
        break;
    }
    case TrayAction::LinkStatus: console({"link", "status"}); break;
    case TrayAction::LinkPair: console({"link", "pair"}); break;
    case TrayAction::StartMcpServer: {
        // one MCP tray per user: its own guard says whether it runs (the menu may be older than that)
        if (services_.mcp_tray && services_.mcp_tray->exists()) {
            notify("The local MCP server is already running (its own tray icon)");
            break;
        }
        const bool ok = actions.start_detached(mcp_tray_args());   // the MCP tray has its own icon, Quit and log
        notify(ok ? "Starting the local MCP server; it gets its own tray icon" : "Could not start the local MCP server");
        break;
    }
    case TrayAction::ToggleAutostart: {
        if (!services_.run_key || services_.autostart.name.empty()) break;
        const bool on = !services_.run_key->get(services_.autostart.name).has_value();
        const bool ok = on ? services_.run_key->set(services_.autostart.name, services_.autostart.line)
                           : services_.run_key->remove(services_.autostart.name);
        if (!ok) notify("Could not change \"Start with Windows\"");
        break;
    }
    case TrayAction::OpenLog: open(paths_.log_file, "the log"); break;
    case TrayAction::OpenAuditFolder: open(paths_.audit_dir, "the audit trail folder"); break;
    case TrayAction::UpdateCheck: console({"update", "check"}); break;
    case TrayAction::Quit:
        // the UI ends once `link run` has returned (run_link_with_tray)
        quit_requested_ = true;
        if (services_.stop_link) services_.stop_link();
        break;
    default: break;   // the MCP server's actions do not exist here
    }
    if (services_.host) services_.host->refresh();
}

int run_link_with_tray(const std::function<int()>& run_link, LinkTrayServices services, SingleInstance& guard,
                       TrayPaths paths) {
    if (!services.host) return 1;
    // started again (Start menu): the running tray opens its menu, as other tray apps come to the front; a tray left
    // running by a removed installation is asked to quit, another installation's is named
    if (!claim_tray(guard, *services.host, services.claim, paths.exe, "fairyfly link", "fairyfly link tray")) return 0;
    const std::string log_file = paths.log_file.string();
    LinkTrayController controller(services, std::move(paths));
    const auto ui = UiThread::start(*services.host, controller);   // ended (quit + join) on every return
    if (!ui) {
        services.host->message_box("fairyfly link", "The fairyfly link tray could not create its tray window. Details are in the log: " +
                                                        log_file);
        return 1;
    }

    // Not paired (e.g. the first start from the Start menu): `link run` would only fail. Stay in the tray, point to
    // "Pair with the fairyfly cloud" and start the link as soon as `link pair` has written the pairing state.
    if (services.link && !services.link->view().paired) {
        spdlog::info("link tray: this desktop is not paired yet; waiting for `fairyfly link pair`");
        spdlog::default_logger()->flush();
        services.host->balloon("fairyfly link", "This desktop is not paired yet. Click the fairyfly icon and choose "
                                                "\"Pair with the fairyfly cloud...\".");
        while (!controller.quit_requested() && !services.link->view().paired)
            std::this_thread::sleep_for(services.pairing_poll);
        if (controller.quit_requested()) return 0;
        spdlog::info("link tray: paired; starting the link");
    }
    // One link per pairing: a console `link run` already serving it would refuse ours (LINK_ALREADY_RUNNING).
    if (services.link_busy && services.link_busy()) {
        spdlog::info("link tray: a link for this pairing already runs elsewhere; waiting until it ends");
        spdlog::default_logger()->flush();
        services.host->balloon("fairyfly link", "A link for this desktop is already running in a console. The tray "
                                                "connects as soon as it ends (Ctrl+C there).");
        while (!controller.quit_requested() && services.link_busy()) std::this_thread::sleep_for(services.pairing_poll);
    }
    if (controller.quit_requested()) return 0;   // Quit right after the pairing appeared (or while another link ran)

    int code = 1;
    try {
        code = run_link();   // the main (COM) thread
    } catch (...) {
        code = 1;
    }
    // A console link claimed the pairing between link_busy() and run_link's own claim (claim_link_run): wait for it
    // as above, then connect.
    while (code == kLinkAlreadyRunningExitCode && services.link_busy && !controller.quit_requested()) {
        spdlog::info("link tray: a console link took this pairing just before ours; waiting until it ends");
        spdlog::default_logger()->flush();
        services.host->balloon("fairyfly link", "A link for this desktop is already running in a console. The tray "
                                                "connects as soon as it ends (Ctrl+C there).");
        do {
            std::this_thread::sleep_for(services.pairing_poll);
        } while (!controller.quit_requested() && services.link_busy());
        if (controller.quit_requested()) {
            code = 0;
            break;
        }
        try {
            code = run_link();
        } catch (...) {
            code = 1;
        }
    }
    if (code != 0 && !controller.quit_requested())
        services.host->message_box("fairyfly link", "The cloud link stopped (exit code " + std::to_string(code) +
                                                        "). Details are in the log: " + log_file);
    return code;
}

} // namespace fairyfly::tray
