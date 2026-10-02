#pragma once

#include "core.h"
#include <chrono>
#include <string>

namespace fairyfly::sap {

/// The SAP SERVER clock for a session.
///
/// SAP GUI Scripting exposes no clock: GuiSessionInfo carries ApplicationServer, Client, Language, Program,
/// ScreenNumber, SystemName, Transaction, User ... but no date, time or time zone, and the status bar panes show
/// system, client, server and mode only. Reading it would need a screen change (System > Status or a transaction),
/// which a session list/attach must never do. So the fields are reported as unavailable, together with the
/// client PC clock as a clearly labelled stand-in (it equals the server time only when both share a time zone).
///
/// {server_time: null, server_time_source: "unavailable", server_time_note, client_time, client_utc_offset}
json server_time_fields_at(std::chrono::system_clock::time_point now, int utc_offset_minutes);

/// Same with the current time and the PC's current UTC offset (DST included).
json server_time_fields();

/// One line for humans and MCP clients: "SAP server time: unavailable ... client PC time 2026-10-02T14:03:11+02:00".
std::string server_time_summary(const json& fields);

/// "+02:00" / "-05:30" / "+00:00".
std::string format_utc_offset(int utc_offset_minutes);

} // namespace fairyfly::sap
