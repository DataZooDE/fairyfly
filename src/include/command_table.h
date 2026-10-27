#pragma once
// Single source of truth for the fairyfly command tree and the MCP tool names derived from it.
// The CLI group creation/help sections, ToolSpec names/families, the audit `command` string, the
// `mcp --tools` filter and `mcp tools` all read this table.

#include <string>
#include <vector>

namespace fairyfly::command_table {

/// One leaf of the noun/verb CLI tree (or a root verb such as `doctor`).
/// What a tool is, for routing, authorization and leases (one place instead of a hand-written list per consumer).
/// Argument-dependent rules (close_session, activate_tab, no_tabs, multiple_logon) stay with their consumers.
enum class Trait : unsigned {
    GlobalControl = 1u << 0,  ///< runs without a routed SAP session (listings, doctor, session setup)
    Targetless = 1u << 1,     ///< needs no session target even for tokens bound to identities/connections
    Listing = 1u << 2,        ///< lists sessions/connections/credentials: filtered for connection-bound tokens
    SessionSystem = 1u << 3,  ///< chooses or opens the SAP system (launch, login, attach, lease)
    LeaseCapable = 1u << 4,   ///< may select tabs, navigate or change the saved session (owner-mode leases)
    WorkerBound = 1u << 5,    ///< runs in a session worker bound to one SAP session
    ScreenBound = 1u << 6,    ///< acts on one established session's screen (owner-mode batch items)
    ActsOnScreen = 1u << 7,   ///< runs inside the transaction open now (T-code allowlists)
    ScreenData = 1u << 8,     ///< returns SAP screen content (results get the "untrusted data" header)
};
constexpr unsigned operator|(Trait a, Trait b) { return static_cast<unsigned>(a) | static_cast<unsigned>(b); }
constexpr unsigned operator|(unsigned a, Trait b) { return a | static_cast<unsigned>(b); }

/// How the CLI and MCP render a successful result of the command as Markdown (format_output).
enum class Render { Generic, ScreenRead, ScreenFind, Doctor };

struct CommandSpec {
    std::vector<std::string> path;  ///< e.g. {"element","click"}
    std::string summary;            ///< one-line description (CLI help)
    std::string help_group;         ///< uppercase root help section, e.g. "ELEMENT"
    std::string tool_name;          ///< MCP tool name ("gui_element_click"); empty = CLI-only
    std::string family;             ///< tool family (the noun; "system" for doctor, "batch" for batch)
    bool mcp_only = false;          ///< broker-native MCP tool with no CLI command
    unsigned traits = 0;            ///< Trait bits
    Render render = Render::Generic;

    bool is_tool() const { return !tool_name.empty(); }
    bool has(Trait t) const { return (traits & static_cast<unsigned>(t)) != 0; }
    std::string path_string() const;  ///< path joined by a space, e.g. "element click"
};

/// A noun group (a CLI11 subcommand that only holds leaves) and its root help section.
struct GroupSpec {
    std::string noun;        ///< "element"
    std::string help_group;  ///< "ELEMENT"
    std::string summary;     ///< help text of the noun
};

/// "gui_" + path joined by "_" (the derivation rule every tool name follows).
std::string derived_tool_name(const std::vector<std::string>& path);

const std::vector<CommandSpec>& all_commands();
/// Noun groups in root-help order.
const std::vector<GroupSpec>& groups();
/// Root help sections in display order (group help sections followed by "SYSTEM").
std::vector<std::string> help_sections();

const CommandSpec* find_by_path(const std::vector<std::string>& path);
const CommandSpec* find_by_tool(const std::string& tool_name);
/// The tool / the CLI command has the trait (false for unknown names).
bool tool_has(const std::string& tool_name, Trait trait);
bool path_has(const std::vector<std::string>& path, Trait trait);
/// The render of the tool / the CLI command (Generic for unknown names).
Render render_of_tool(const std::string& tool_name);
Render render_of_path(const std::vector<std::string>& path);
const GroupSpec* find_group(const std::string& noun);
/// Unique tool families in table order.
std::vector<std::string> families();

/// Audit command string of an argv (without program name): the longest table path that prefixes
/// the argv, joined by a space ("element click"); falls back to argv[0] ("" for an empty argv).
std::string command_of_argv(const std::vector<std::string>& argv);

/// Splits a `--tools` value (comma and/or whitespace separated) into lower-case family names.
std::vector<std::string> parse_family_list(const std::string& text);
/// Family names in `families` that no tool belongs to.
std::vector<std::string> unknown_families(const std::vector<std::string>& families);

} // namespace fairyfly::command_table
