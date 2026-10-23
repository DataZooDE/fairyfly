#pragma once
// Which SAP system a GUI session belongs to, as JSON for `session list` / gui_session_list and the
// `screen read` / gui_screen_read header. Everything comes from GuiSessionInfo (SystemName, Client, User,
// Transaction, Language, ApplicationServer), which has no credential member: no screen field, and so never
// a password field's value, is read here.

#include <string>

#include <nlohmann/json.hpp>

#include "include/com/wrapper.h"

namespace fairyfly::sap {

/// GuiSessionInfo read once; each member read on its own so one unreadable member leaves only that field empty.
inline nlohmann::json read_session_info_fields(const ComGuiSession& session, bool details) {
    nlohmann::json out = nlohmann::json::object();
    IDispatchPtr info;
    try { info = session.get_dispatch_property(L"Info"); } catch (...) {}
    const auto read = [&](const char* key, const wchar_t* member) {
        std::string value;
        if (info) {
            try { value = SapGuiObject(info).get_string_property(member); } catch (...) {}
        }
        out[key] = value;
    };
    read("system", L"SystemName");
    read("client", L"Client");
    read("user", L"User");
    read("transaction", L"Transaction");
    if (details) {
        read("language", L"Language");
        read("app_server", L"ApplicationServer");
    }
    return out;
}

/// One `sessions[]` entry's identity fields: system, client, user (empty before logon), transaction,
/// language, app_server, plus `description` (the SAP Logon connection description). A busy session is not
/// asked for Info (the read would block until the server round trip ends): its six Info fields are null.
inline nlohmann::json session_list_info(const ComGuiSession& session, const std::string& connection_description) {
    bool busy = false;
    try { busy = session.is_busy(); } catch (...) {}
    nlohmann::json out;
    if (busy) {
        for (const char* key : {"system", "client", "user", "transaction", "language", "app_server"}) out[key] = nullptr;
    } else {
        out = read_session_info_fields(session, true);
    }
    out["description"] = connection_description;
    return out;
}

/// The `screen read` header fields: system, client, user, transaction.
inline nlohmann::json screen_header_info(const ComGuiSession& session) {
    return read_session_info_fields(session, false);
}

} // namespace fairyfly::sap
