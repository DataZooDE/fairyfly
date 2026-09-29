#pragma once
// Single source of truth for the fairyfly command tree and the MCP tool names derived from it.
// The CLI group creation/help sections, ToolSpec names/families, the audit `command` string, the
// `mcp --tools` filter and `mcp tools` all read this table.

#include <string>
#include <vector>

namespace fairyfly::command_table {

/// One leaf of the noun/verb CLI tree (or a root verb such as `doctor`).
struct CommandSpec {
    std::vector<std::string> path;  ///< e.g. {"element","click"}
    std::string summary;            ///< one-line description (CLI help)
    std::string help_group;         ///< uppercase root help section, e.g. "ELEMENT"
    std::string tool_name;          ///< MCP tool name ("gui_element_click"); empty = CLI-only
    std::string family;             ///< tool family (the noun; "system" for doctor, "batch" for batch)

    bool is_tool() const { return !tool_name.empty(); }
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
