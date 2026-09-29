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
    else if (status.running) line = "Running: " + endpoint_text(status) + " (" + mode_text(status) + ")";
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
    model.items.push_back(separator());

    model.items.push_back(item("Quit", TrayAction::Quit));

    std::string tip = "fairyfly MCP: ";
    if (!state.last_error.empty()) tip += "error";
    else if (!status.running) tip += "stopped";
    else tip += "running (" + mode_text(status) + ")";
    if (status.running && !status.warnings.empty()) tip += ", " + std::to_string(status.warnings.size()) + " warning(s)";
    if (tip.size() > 127) tip.resize(127);
    model.tooltip = std::move(tip);
    return model;
}

std::string status_balloon_text(const mcp::ServerStatus& status) {
    std::string text = status.running ? "Running: " : "Stopped: ";
    text += endpoint_text(status) + " (" + mode_text(status) + ")";
    text += "\nCalls: " + std::to_string(status.calls_total) + ", denied: " + std::to_string(status.calls_denied);
    for (const auto& w : status.warnings) text += "\n! " + w;
    return text;
}

} // namespace fairyfly::tray
