// The link tray (`fairyfly link run --tray`): the cloud link on its own, without the MCP server.
#include <thread>

#include "include/tray/tray.h"

namespace fairyfly::tray {

namespace {

MenuItem item(std::string label, TrayAction action, bool enabled = true) {
    MenuItem m;
    m.label = std::move(label);
    m.action = action;
    m.enabled = enabled;
    return m;
}

MenuItem separator() {
    MenuItem m;
    m.separator = true;
    return m;
}

std::string who(const LinkView& link) { return link.organisation + " (" + link.account + ")"; }

} // namespace

TrayMenuModel build_link_menu(const LinkView& link, const LinkWritesView& writes) {
    TrayMenuModel model;
    if (link.state == "connected") model.icon = IconState::Running;
    else if (link.state == "paused") model.icon = IconState::Warning;
    else model.icon = IconState::Stopped;

    if (link.paired) {
        model.items.push_back(item(who(link) + ": " + link.state, TrayAction::None, false));
        model.items.push_back(separator());
        if (link.paused) model.items.push_back(item("Resume", TrayAction::LinkResume));
        else model.items.push_back(item("Pause", TrayAction::LinkPause));
        if (writes.available) {
            MenuItem allow = item(writes.locked ? "Allow changes in SAP (off: FAIRYFLY_READ_ONLY / --read-only)"
                                                : "Allow changes in SAP",
                                  TrayAction::LinkToggleWrites, !writes.locked);
            allow.checkable = true;
            allow.checked = writes.allowed && !writes.locked;
            model.items.push_back(std::move(allow));
        }
        model.items.push_back(item("Health check (console)", TrayAction::LinkStatus));
        model.items.push_back(item("Pair again (console)", TrayAction::LinkPair));
    } else {
        model.items.push_back(item("Not connected to the fairyfly cloud", TrayAction::None, false));
        model.items.push_back(separator());
        model.items.push_back(item("Connect to cloud... (pair in a console)", TrayAction::LinkPair));
    }
    model.items.push_back(separator());
    model.items.push_back(item("Open log", TrayAction::OpenLog));
    model.items.push_back(item("Open audit trail folder", TrayAction::OpenAuditFolder));
    model.items.push_back(item("Check for updates (console)", TrayAction::UpdateCheck));
    model.items.push_back(separator());
    model.items.push_back(item("Quit", TrayAction::Quit));

    std::string tooltip = "fairyfly link: " + link.state;
    if (writes.available) tooltip += writes.allowed && !writes.locked ? " (changes allowed)" : " (read-only)";
    if (link.paired) tooltip += " - " + who(link);
    if (tooltip.size() > 127) tooltip = tooltip.substr(0, 124) + "...";
    model.tooltip = tooltip;
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
    return v;
}

TrayMenuModel LinkTrayController::menu() { return build_link_menu(link_, writes_view()); }

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
    const std::string state = link_.state;
    if (state == last_state_) return;
    // "not running" before the first status is written (start) or after Quit says nothing new
    if (state == "connected") notify(last_state_ == "paused" ? "Cloud link resumed" : "Cloud link running for " + who(link_));
    else if (state == "paused") notify("Cloud link paused: the AI assistant cannot reach this desktop");
    else if (!last_state_.empty() && !quit_requested_) notify("Cloud link stopped");
    last_state_ = state;
}

void LinkTrayController::on_action(TrayAction action) {
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
    case TrayAction::LinkPause:
    case TrayAction::LinkResume: {
        const bool pause = action == TrayAction::LinkPause;
        if (services_.link && services_.link->set_paused(pause)) link_.paused = pause;   // the notice follows on the tick
        else notify(pause ? "Could not pause the cloud link" : "Could not resume the cloud link");
        break;
    }
    case TrayAction::LinkToggleWrites: {
        if (!services_.writes || services_.writes->locked()) break;
        const bool allow = !services_.writes->allowed();
        if (allow && services_.host &&
            !services_.host->confirm("fairyfly link",
                                     "Allow AI assistants to change data in SAP through the cloud link?\n\nEach change still "
                                     "needs your organisation's grant and an approval (this desktop's dialog or the phone)."))
            break;
        if (!services_.writes->set_allowed(allow)) notify("Could not change the link's mode");
        else notify(allow ? "Changes in SAP allowed; the link reconnects with the write tools"
                          : "Read-only: the link reconnects without the write tools");
        break;
    }
    case TrayAction::LinkStatus: console({"link", "status"}); break;
    case TrayAction::LinkPair: console({"link", "pair"}); break;
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
    if (!guard.acquire()) {
        services.host->message_box("fairyfly link", "The fairyfly link tray is already running.");
        return 0;
    }
    const std::string log_file = paths.log_file.string();
    LinkTrayController controller(services, std::move(paths));
    std::thread ui;
    if (!detail::start_ui(*services.host, controller, ui)) return 1;

    int code = 1;
    try {
        code = run_link();   // the main (COM) thread
    } catch (...) {
        code = 1;
    }
    if (code != 0 && !controller.quit_requested())
        services.host->message_box("fairyfly link", "The cloud link stopped (exit code " + std::to_string(code) +
                                                        "). Details are in the log: " + log_file);
    services.host->post_quit();
    ui.join();
    return code;
}

} // namespace fairyfly::tray
