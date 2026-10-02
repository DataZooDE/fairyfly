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

/// Tool families that act on whatever transaction is open now (screen, element, key, popup, menu), as opposed to
/// choosing a new one (gui_transaction_start) or managing sessions.
bool acts_on_screen(const std::string& family);

/// Keys gui_key_send may use for a token with a T-code allowlist that was NOT created with --allow-navigation:
/// Enter (0), F4 (4), F8 (8) and the page keys (VKeys 80 Ctrl+PageUp/page top, 81 PageUp, 82 PageDown, 83 Ctrl+PageDown/page bottom; named pageup, pagedown, pagetop, pagebottom or raw numbers). Everything else (F3 back, F12 cancel, Shift+F3 exit,
/// F5/F6/F7, Ctrl+... and unparsable input) can leave the transaction and is refused. Accepts every spelling of
/// the key parser (enter, f4, F8, "80", ...).
bool tcode_safe_key(const std::string& key);

/// The exact spellings of the keys tcode_safe_key accepts, as listed in the denial message.
std::string tcode_safe_key_spellings();

/// True when `element_id` is the SAP command field (".../okcd", e.g. wnd[0]/tbar[0]/okcd).
bool is_okcd_element(const std::string& element_id);

/// The screen a token's last successful gui_transaction_start ended on (selection-input rule).
struct InitialScreen {
    std::string transaction;     ///< normalized T-code that was open after the start
    std::string program;         ///< Info.Program
    std::string screen_number;   ///< Info.ScreenNumber
    std::optional<int> connection; ///< connection the start targeted (explicit, sticky or default); typing must target the same one
    bool known() const { return !transaction.empty() && !program.empty() && !screen_number.empty(); }
};

/// What the dispatcher knows about the screen at the time of the call (the selection-input rule only).
struct SelectionInputContext {
    std::string program;                  ///< pre-call facts: current Info.Program ("" = unknown)
    std::string screen_number;            ///< pre-call facts: current Info.ScreenNumber ("" = unknown)
    std::optional<int> connection;        ///< connection this fill targets (explicit, sticky or default as the dispatcher resolves it)
    std::optional<InitialScreen> initial; ///< recorded by the principal's last successful gui_transaction_start
};

/// True when `tool` is gui_element_fill, the principal has allow_selection_input and either the token or the server is
/// read-only: the call then takes the selection-input path (the read-only refusal is replaced by the INPUT_* rules).
bool selection_input_path(const mcp::Principal& principal, const std::string& tool, const mcp::Policy& server_policy);

/// Whether tools/list shows gui_element_fill to a read-only principal with allow_selection_input (scope + T-code allowlist).
bool selection_input_tool_visible(const mcp::Principal& principal, const mcp::ToolSpec& spec);

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
///    the command field (okcd) is refused too. Unless the token has allow_navigation, gui_menu_select is denied and
///    gui_key_send is limited to tcode_safe_key() keys (fail closed; menu paths and F3/F12/Shift+F3 leave the
///    transaction). With allow_navigation both stay usable and the dispatcher's post-call re-check is the only net.
///  - T-code allowlist, current transaction: with an allowlist, every tool of the families screen, element, key, popup and
///    menu also requires `current_tcode` (the transaction open now, same normalisation and glob rules) to be allowlisted;
///    unknown/empty (or S000/SESSION_MANAGER unless allowlisted) -> TCODE_DENIED. It can still change during a call.
///  - gui_batch: every item is checked with the static rules (scope, read-only, T-code names); the system rule and the
///    current-transaction rule are applied per item by the dispatcher at execution time because an earlier item may
///    attach the session or start another transaction.
///  - selection input (principal.allow_selection_input, see selection_input_path): gui_element_fill on a read-only token/
///    server is allowed ONLY when the token has a T-code allowlist, the open transaction is allowlisted, the current
///    (program, screen number) and the connection equal `input.initial` exactly (unknown = deny) and the target is a plain input field
///    (no row/column/checkbox/commit, not the command field, not a password/credential field). Refusals:
///    INPUT_NOT_ALLOWED (option set but no T-code allowlist), INPUT_SCREEN_DENIED, INPUT_TARGET_DENIED; an open
///    transaction outside the allowlist keeps TCODE_DENIED. For gui_batch items only the static target rules run here
///    (the screen rule is applied per item by the dispatcher).
mcp::PolicyDecision authorize_call(const mcp::Principal& principal, const mcp::ToolSpec& spec, const std::string& family,
                                   const mcp::json& args, const mcp::Policy& server_policy,
                                   std::optional<std::string> current_system, std::optional<std::string> current_tcode,
                                   const SpecLookup& lookup = {}, const SelectionInputContext* input = nullptr);

/// What a session/connection-targeting call is about to act on, as far as it could be determined WITHOUT contacting
/// SAP (saved connection files, the live session's own metadata). Empty fields mean "not determinable".
struct SessionTarget {
    std::string system;           ///< "SID/CLIENT" (or just "SID" when the client is not known yet)
    std::string connection_name;  ///< live connection description / saved connection / SAP Logon entry name
    bool ambiguous = false;       ///< launch: open sessions of the entry name run on different systems
};

/// True when `tool` must be checked against the target (see authorize_session_target) for this principal, so the
/// dispatcher can skip the lookup for everybody else (tokens without sap_systems/connections are unchanged).
bool needs_session_target(const mcp::Principal& principal, const std::string& tool, const mcp::json& args);

/// Enforces the token's SAP-system allowlist and `connections` allowlist on the calls that choose or end a session:
///  - gui_session_login / gui_session_attach, and gui_session_disconnect with close_session:
///    with sap_systems set the target system must be determinable and allowed -> SYSTEM_UNKNOWN (fail closed) /
///    SYSTEM_DENIED.
///  - gui_session_launch (also with login=true) with sap_systems set: the SAP system of a SAP Logon entry cannot be
///    known before it is opened, and facts of OTHER sessions are no proof (an entry can be renamed or repointed, two
///    entries can share a description). It is therefore allowed only when the token ALSO has a `connections` glob that
///    matches the entry name (the operator vouches that the name maps to an allowed system) -> otherwise
///    SYSTEM_UNKNOWN. When sessions of that name are open, their system must additionally be allowed (SYSTEM_DENIED;
///    ambiguous -> SYSTEM_UNKNOWN). Every later call is checked against the real system facts.
///  - with `connections` set, every tool that acts on a connection (launch, login, attach, disconnect and the
///    screen/element/key/... tools) needs the connection name to be determinable and to match one glob
///    -> CONNECTION_DENIED. gui_session_list, gui_connection_list, gui_credentials_list, gui_doctor and
///    gui_batch (its items are checked one by one) have no target; the three listings are limited through
///    filter_listing_for_connections instead. gui_connection_list cleanup=true is refused by authorize_call.
/// Pure; tokens without either list are always allowed.
mcp::PolicyDecision authorize_session_target(const mcp::Principal& principal, const std::string& tool, const mcp::json& args,
                                             const SessionTarget& target);

/// True for the three listing tools whose RESULT must be limited to the token's `connections` (gui_session_list,
/// gui_connection_list, gui_credentials_list) when the token has a connections restriction.
bool listing_needs_filter(const mcp::Principal& principal, const std::string& tool);

/// Filters the structured Result of such a listing in place, before shaping and audit:
///  - gui_session_list: connections whose LIVE description does not match (or has none) are dropped with their
///    sessions; total_connections, total_sessions and the enumeration error counters are recomputed from what is kept.
///  - gui_connection_list: rows are matched by `description`; gui_credentials_list by `connection`; `count` is recomputed.
///  - an error result keeps only its code (the message may name other connections) and loses diagnostics.
/// Returns false when the data does not have the expected shape (unknown top-level keys, wrong types): the data is
/// cleared and the caller must return an error instead of the listing. Other tools and unrestricted tokens: no-op, true.
bool filter_listing_for_connections(const mcp::Principal& principal, const std::string& tool, Result& result);

/// The token's own calls-per-minute limit for a tool family (`--rate-family element=10`); 0 = none.
int rate_family_limit(const mcp::Principal& principal, const std::string& family);

/// Whether tools/list should show `spec` to `principal` (scope + read-only). Pure.
bool tool_allowed_for(const mcp::Principal& principal, const mcp::ToolSpec& spec);

} // namespace fairyfly::auth
