#pragma once
// Where the SAP GUI windows of an MCP server may live.

#include <string>
#include <vector>

namespace fairyfly::mcp {

/// Whether the server may fall back to an embedded SapGui.ScriptingCtrl when SAP Logon is not running. The embedded
/// control hosts the SAP GUI windows inside the server process, where they are invisible to other processes through
/// the running object table. Owner-mode HTTP endpoints route calls to session worker processes that attach exactly
/// that way, so for them such windows would open and log on but could never be driven (OWNER_IDENTITY_UNKNOWN):
/// they refuse the fallback and report SAP_LOGON_NOT_RUNNING instead.
inline bool embedded_sap_gui_allowed(bool http, const std::vector<std::string>& owner_sap_identities) {
    return !(http && !owner_sap_identities.empty());
}

} // namespace fairyfly::mcp
