#pragma once
// Token scopes, one pure place. A scope string is
//   "*"                 every tool
//   "<family>"          every tool of the family (the CLI noun; "system", "batch")
//   "<family>.<verb>"   exactly one tool; verb = last element of the tool's CLI path in the command table
// The valid set is derived from src/command_table.cpp (nothing is hard-coded here). Roots without a verb
// (doctor, batch) have family-only scopes.

#include <optional>
#include <set>
#include <string>
#include <vector>

namespace fairyfly::auth {

/// Lower-case form of a scope as typed by an operator.
std::string normalize_scope(std::string scope);

/// The verb scope of a tool ("gui_session_disconnect" -> "session.disconnect"); empty when the tool has none
/// (gui_doctor, gui_batch, unknown tools).
std::string verb_scope_of_tool(const std::string& tool_name);

/// Every valid "<family>.<verb>" scope, in command table order.
std::vector<std::string> verb_scopes();
/// The verbs of one family ("session" -> list, attach, launch, login, disconnect); empty for unknown/verbless families.
std::vector<std::string> verbs_of_family(const std::string& family);

/// Why a scope is not acceptable: code UNKNOWN_FAMILY or UNKNOWN_SCOPE plus an operator-facing message.
struct ScopeError {
    std::string code;
    std::string message;
};

/// Validates one (already normalized) scope. nullopt = valid.
std::optional<ScopeError> validate_scope(const std::string& scope);

/// Whether `granted` (raw scope strings of a principal, `all` = "*") allows the tool of `family`.
bool scopes_allow(const std::set<std::string>& granted, bool all, const std::string& family, const std::string& tool_name);

/// The scope names that would allow the tool, for SCOPE_DENIED messages: "'session.disconnect' (or 'session')", or
/// just "'system'" for tools without a verb scope.
std::string missing_scope_hint(const std::string& family, const std::string& tool_name);

/// The scope table of docs/MCP.md (between the scope-table markers; a unit test keeps it in sync).
std::string scope_table_markdown();

/// Help text for `mcp token create --scope`: families plus the verb form generated from the command table.
std::string scope_help_text();

} // namespace fairyfly::auth
