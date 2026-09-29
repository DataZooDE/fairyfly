#pragma once
// Pure per-call authorization: does this principal's token allow this tool call?
// No I/O, no clock, no globals: unit-testable as a table.

#include <functional>
#include <optional>
#include <string>

#include "include/mcp/types.h"

namespace fairyfly::auth {

/// Case-insensitive glob ('*' any run, '?' one character).
bool glob_match(const std::string& pattern, const std::string& text);

/// "/nSE16 ", "/o se16", "/*VA01" -> "SE16"/"VA01": trims, upper-cases, strips the /n /o /* prefix and any
/// parameters after the first blank. Other slash commands keep their slash ("/H", "/I") so they never
/// match a plain allowlist entry.
std::string normalize_tcode(const std::string& code);

/// True when `element_id` is the SAP command field (".../okcd", e.g. wnd[0]/tbar[0]/okcd).
bool is_okcd_element(const std::string& element_id);

/// Looks up a tool by name (used for gui_batch items). Empty = the built-in catalog.
using SpecLookup = std::function<const mcp::ToolSpec*(const std::string& tool_name)>;

/// Decides whether `principal` may run `spec` with `args`.
///  - scope: `family` must be one of the token's scopes (or the token has "*") -> SCOPE_DENIED
///  - read-only: a read_only token is refused write/destructive calls with the server read-only rule set -> READ_ONLY
///    (effective read-only = server || token; the dispatcher runs check_call first so the server's own refusals keep their codes)
///  - SAP system/client allowlist against `current_system` ("SID/CLIENT"; nullopt = unknown) -> SYSTEM_DENIED /
///    SYSTEM_UNKNOWN. Session/connection/system/credentials tools and gui_batch itself are exempt HERE (they
///    run before any system is attached; launch/login/attach/disconnect are checked against THEIR target by
///    authorize_session_target; every later call is checked).
///  - T-code allowlist: gui_transaction_start `code` -> TCODE_DENIED; while an allowlist is set, gui_element_fill into
///    the command field (okcd) is refused too. gui_key_send and menus are NOT blocked (residual risk, see docs/MCP.md).
///  - T-code allowlist, current transaction: with an allowlist, every tool of the families screen, element, key, popup and
///    menu also requires `current_tcode` (the transaction open now, same normalisation and glob rules) to be allowlisted;
///    unknown/empty (or S000/SESSION_MANAGER unless allowlisted) -> TCODE_DENIED. It can still change during a call.
///  - gui_batch: every item is checked with the static rules (scope, read-only, T-code names); the system rule and the
///    current-transaction rule are applied per item by the dispatcher at execution time because an earlier item may
///    attach the session or start another transaction.
mcp::PolicyDecision authorize_call(const mcp::Principal& principal, const mcp::ToolSpec& spec, const std::string& family,
                                   const mcp::json& args, const mcp::Policy& server_policy,
                                   std::optional<std::string> current_system, std::optional<std::string> current_tcode,
                                   const SpecLookup& lookup = {});

/// What a session/connection-targeting call is about to act on, as far as it could be determined WITHOUT contacting
/// SAP (saved connection files, the live session's own metadata). Empty fields mean "not determinable".
struct SessionTarget {
    std::string system;           ///< "SID/CLIENT" (or just "SID" when the client is not known yet)
    std::string connection_name;  ///< saved connection description / SAP Logon entry name
};

/// True when `tool` must be checked against the target (see authorize_session_target) for this principal, so the
/// dispatcher can skip the lookup for everybody else (tokens without sap_systems/connections are unchanged).
bool needs_session_target(const mcp::Principal& principal, const std::string& tool, const mcp::json& args);

/// Enforces the token's SAP-system allowlist and `connections` allowlist on the calls that choose or end a session:
///  - gui_session_launch / gui_session_login / gui_session_attach, and gui_session_disconnect with close_session:
///    with sap_systems set the target system must be determinable and allowed -> SYSTEM_UNKNOWN (fail closed,
///    e.g. a SAP Logon entry that has no open session yet) / SYSTEM_DENIED.
///  - with `connections` set, every tool that acts on a connection (launch, login, attach, disconnect and the
///    screen/element/key/... tools) needs the connection name to be determinable and to match one glob
///    -> CONNECTION_DENIED. gui_session_list, gui_connection_list, gui_credentials_list, gui_doctor and
///    gui_batch (its items are checked one by one) have no target and are not restricted.
/// Pure; tokens without either list are always allowed.
mcp::PolicyDecision authorize_session_target(const mcp::Principal& principal, const std::string& tool, const mcp::json& args,
                                             const SessionTarget& target);

/// Whether tools/list should show `spec` to `principal` (scope + read-only). Pure.
bool tool_allowed_for(const mcp::Principal& principal, const mcp::ToolSpec& spec);

} // namespace fairyfly::auth
