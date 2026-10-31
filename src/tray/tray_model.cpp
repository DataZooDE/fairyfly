#include "include/link/link_state_names.h"
#include "include/tray/mcp_tray.h"
#include "include/version.h"

namespace fairyfly::tray {
void append_sap_startup_menu(TrayMenuModel& menu, TraySettings* settings) {
    if (!settings) return;
    auto entry = item("Allow automatic SAP startup", TrayAction::ToggleSapStartup);
    entry.checkable = true;
    entry.checked = settings->get(kAllowSapStartupSetting).value_or("0") == "1";
    menu.items.insert(menu.items.begin(), std::move(entry));
}

bool toggle_sap_startup(TraySettings* settings) {
    if (!settings) return false;
    const bool enabled = settings->get(kAllowSapStartupSetting).value_or("0") == "1";
    return settings->set(kAllowSapStartupSetting, enabled ? "0" : "1");
}

MenuItem item(std::string label, TrayAction action, bool enabled) {
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

std::string fit_tooltip(const std::string& text) {
    constexpr size_t kMaxUnits = 127;  // NOTIFYICONDATA szTip: 128 wchar_t with the terminator
    // UTF-16 units of each character: 2 for a 4-byte UTF-8 sequence (outside the BMP), else 1
    const auto lead = [](unsigned char c) { return (c & 0xC0) != 0x80; };
    const auto units = [](unsigned char c) { return c >= 0xF0 ? size_t{2} : size_t{1}; };
    size_t total = 0;
    for (unsigned char c : text) if (lead(c)) total += units(c);
    if (total <= kMaxUnits) return text;
    size_t used = 0, end = 0;
    for (size_t i = 0; i < text.size(); ++i) {
        const auto c = static_cast<unsigned char>(text[i]);
        if (!lead(c)) continue;
        if (used + units(c) > kMaxUnits - 3) break;  // room for "..."
        used += units(c);
        end = i + 1;
        while (end < text.size() && !lead(static_cast<unsigned char>(text[end]))) ++end;
    }
    return text.substr(0, end) + "...";
}

MenuItem version_item() { return item(std::string("fairyfly ") + FAIRYFLY_VERSION, TrayAction::None, false); }

namespace {

std::string mode_text(const mcp::ServerStatus& status) { return status.read_only ? "read-only" : "write mode"; }

std::string endpoint_text(const mcp::ServerStatus& status) { return status.endpoint.empty() ? "no endpoint" : status.endpoint; }

} // namespace

IconState icon_for(const mcp::ServerStatus& status, const TrayState& state) {
    if (!state.last_error.empty()) return IconState::Error;
    if (!status.running) return IconState::Stopped;
    if (!status.warnings.empty()) return IconState::Warning;
    return IconState::Running;
}

TrayMenuModel build_menu(const mcp::ServerStatus& status, const TrayState& state) {
    TrayMenuModel model;
    model.icon = icon_for(status, state);

    // Status line.
    std::string line;
    if (!state.last_error.empty()) line = "Error: " + state.last_error;
    else if (status.running) line = "Running: " + endpoint_text(status) + " (" + mode_text(status) +
        (status.parallel_sessions ? ", parallel SAP windows" : "") + ")";
    else line = "Stopped: " + endpoint_text(status) + " (" + mode_text(status) + ")";
    model.items.push_back(item(line, TrayAction::None, false));

    // Warnings submenu.
    if (status.warnings.empty()) {
        model.items.push_back(item("No warnings", TrayAction::None, false));
    } else {
        MenuItem warnings = item("Warnings (" + std::to_string(status.warnings.size()) + ")", TrayAction::None);
        for (const auto& w : status.warnings) warnings.children.push_back(item(w, TrayAction::None, false));
        model.items.push_back(std::move(warnings));
    }
    model.items.push_back(separator());

    // Server control.
    model.items.push_back(item("Start", TrayAction::Start, !status.running));
    model.items.push_back(item("Stop", TrayAction::Stop, status.running));
    model.items.push_back(item("Restart", TrayAction::Restart, status.running));

    MenuItem read_only = item("Read-only mode", TrayAction::ToggleReadOnly);
    read_only.checkable = true;
    read_only.checked = status.read_only || state.hard_read_only;
    if (state.hard_read_only) {
        read_only.label = "Read-only mode (forced by FAIRYFLY_READ_ONLY)";
        read_only.enabled = false;
    }
    model.items.push_back(std::move(read_only));
    model.items.push_back(separator());

    model.items.push_back(item("Open log", TrayAction::OpenLog));
    model.items.push_back(item("Open audit trail folder", TrayAction::OpenAuditFolder));
    model.items.push_back(item("Open config file", TrayAction::OpenConfig));

    MenuItem tokens = item("Tokens", TrayAction::None);
    tokens.children.push_back(item("List tokens (console)", TrayAction::TokenList));
    tokens.children.push_back(item("Create token (console)", TrayAction::TokenCreate));
    model.items.push_back(std::move(tokens));

    // Cloud link (D10): the outbound connection to the fairyfly cloud gateway, a separate `fairyfly link run`.
    MenuItem cloud = item("Cloud link", TrayAction::None);
    const LinkView& lv = state.link;
    if (!lv.paired) {
        cloud.children.push_back(item("Not connected to the fairyfly cloud", TrayAction::None, false));
        cloud.children.push_back(item("Connect to cloud... (pair in a console)", TrayAction::LinkPair));
    } else {
        cloud.children.push_back(item(lv.organisation + " (" + lv.account + "): " + lv.state, TrayAction::None, false));
        if (lv.trail_on) cloud.children.push_back(item(trail_menu_text(lv), TrayAction::None, false));
        cloud.children.push_back(item("Start link", TrayAction::LinkStart, lv.state == link::state::kNotRunning));
        if (lv.paused) cloud.children.push_back(item("Resume", TrayAction::LinkResume));
        else cloud.children.push_back(item("Pause", TrayAction::LinkPause));
        cloud.children.push_back(item("Health check (console)", TrayAction::LinkStatus));
        cloud.children.push_back(item("Pair again (console)", TrayAction::LinkPair));
    }
    model.items.push_back(std::move(cloud));
    model.items.push_back(version_item());
    if (state.self_update) model.items.push_back(item("Check for updates (console)", TrayAction::UpdateCheck));
    model.items.push_back(separator());

    model.items.push_back(item("Quit", TrayAction::Quit));

    std::string tip = "fairyfly MCP: ";
    if (!state.last_error.empty()) tip += "error";
    else if (!status.running) tip += "stopped";
    else tip += "running (" + mode_text(status) +
        (status.parallel_sessions ? ", parallel sessions" : "") + ")";
    if (status.running && !status.warnings.empty()) tip += ", " + std::to_string(status.warnings.size()) + " warning(s)";
    model.tooltip = fit_tooltip(tip);
    return model;
}

std::string trail_menu_text(const LinkView& link) {
    if (!link.trail_on) return {};
    std::string s = "Audit trail on \xE2\x80\x94 your admin records steps and screenshots on this desktop";
    if (link.trail_mode == "changes") s += " (screenshots of changes)";
    return s;
}

std::string clock_skew_text(const LinkView& link) {
    if (link.clock_skew_s == 0) return {};
    const auto d = link.clock_skew_s < 0 ? -link.clock_skew_s : link.clock_skew_s;
    return "Windows clock is " + std::to_string(d) + " s " + (link.clock_skew_s < 0 ? "ahead of" : "behind") +
           " the fairyfly cloud: calls are refused. Settings > Time & language > Date & time > Sync now";
}

std::string stuck_call_text(const LinkView& link) {
    if (link.stuck_tool.empty()) return {};
    return "SAP GUI has not answered " + link.stuck_tool +
           (link.stuck_connection.empty() ? std::string() : " on connection " + link.stuck_connection) + " for " +
           std::to_string(link.stuck_s) + " s: look for a hidden SAP dialog or a long-running step (e.g. a report started with F8), or end that SAP session";
}

std::string trail_balloon_text(const LinkView& link) {
    if (!link.trail_on) return "Audit trail off: the steps assistants take on this desktop are no longer recorded";
    return "Audit trail on: your administrator records the steps AI assistants take on this desktop, with screenshots" +
           std::string(link.trail_mode == "changes" ? " of the steps that change SAP" : "") + " (kept " +
           std::to_string(link.trail_retention_days) + " days)";
}

std::string status_balloon_text(const mcp::ServerStatus& status) {
    std::string text = status.running ? "Running: " : "Stopped: ";
    text += endpoint_text(status) + " (" + mode_text(status) + ")";
    text += "\nCalls: " + std::to_string(status.calls_total) + ", denied: " + std::to_string(status.calls_denied);
    if (status.running && status.parallel_sessions)
        text += "\nOwner-restricted sessions: same-window calls ordered; different windows concurrent";
    for (const auto& w : status.warnings) text += "\n! " + w;
    return text;
}

} // namespace fairyfly::tray
