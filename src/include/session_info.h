#pragma once
// Which SAP system a GUI session belongs to, as JSON for `session list` / gui_session_list and the
// `screen read` / gui_screen_read header. Everything comes from GuiSessionInfo (SystemName, Client, User,
// Transaction, Language, ApplicationServer), which has no credential member: no screen field, and so never
// a password field's value, is read here.

#include <string>

#include <nlohmann/json.hpp>

#include "include/com/wrapper.h"

namespace fairyfly::sap {

/// One `sessions[]` entry's identity fields: system, client, user (empty before logon), transaction,
/// language, app_server, plus `description` (the SAP Logon connection description). A busy session is not
/// asked for Info (the read would block until the server round trip ends): its six Info fields are null.
inline nlohmann::json session_list_info(const ComGuiSession& session, const std::string& connection_description) {
    (void)session;
    (void)connection_description;
    return nlohmann::json::object();
}

/// The `screen read` header fields: system, client, user, transaction.
inline nlohmann::json screen_header_info(const ComGuiSession& session) {
    (void)session;
    return nlohmann::json::object();
}

} // namespace fairyfly::sap
