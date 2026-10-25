#include "include/command_table.h"

#include "include/string_utils.h"
#include <algorithm>
#include <cctype>

namespace fairyfly::command_table {

namespace {

struct Row {
    std::vector<std::string> path;
    const char* summary;
    const char* help_group;
    bool tool;
    const char* family_override;  // nullptr = the noun
    bool mcp_only = false;
    unsigned traits = 0;
    Render render = Render::Generic;
};

// the session-bound tools: one worker, one established session's screen
constexpr unsigned kOnScreen = Trait::WorkerBound | Trait::ScreenBound | Trait::ActsOnScreen;
constexpr unsigned kOnScreenLeased = kOnScreen | Trait::LeaseCapable;
constexpr unsigned kListing = Trait::GlobalControl | Trait::Targetless | Trait::Listing;
constexpr unsigned kSessionSetup = Trait::GlobalControl | Trait::SessionSystem;

std::string join(const std::vector<std::string>& parts, const std::string& sep) {
    std::string out;
    for (const auto& part : parts) {
        if (!out.empty()) out += sep;
        out += part;
    }
    return out;
}

const std::vector<Row>& rows() {
    static const std::vector<Row> table = {
        {{"session", "list"}, "List all SAP GUI connections, sessions, and windows", "SESSION", true, nullptr, false, kListing},
        {{"session", "attach"}, "Attach to a running SAP GUI window or exact session ID", "SESSION", true, nullptr, false, kSessionSetup},
        {{"session", "launch"}, "Launch SAP Logon connection", "SESSION", true, nullptr, false, kSessionSetup},
        {{"session", "login"}, "Log into a launched SAP GUI session", "SESSION", true, nullptr, false, kSessionSetup},
        {{"session", "disconnect"}, "Remove a saved connection; optionally close its SAP GUI session", "SESSION", true, nullptr, false, Trait::WorkerBound | Trait::LeaseCapable},
        {{"session", "lease"}, "Acquire, renew or release an MCP session lease", "SESSION", true, nullptr, true, kSessionSetup},
        {{"connection", "list"}, "List and manage saved fairyfly connections", "CONNECTION", true, nullptr, false, kListing},
        {{"screen", "read"}, "Read screen structure (max 500 elements)", "SCREEN", true, nullptr, false, kOnScreenLeased | Trait::ScreenData, Render::ScreenRead},
        {{"screen", "find"}, "Find visible controls without reading unrelated values", "SCREEN", true, nullptr, false, kOnScreen | Trait::ScreenData, Render::ScreenFind},
        {{"screen", "capture"}, "Capture screenshot", "SCREEN", true, nullptr, false, kOnScreen},
        {{"menu", "list"}, "List the menu bar tree", "MENU", true, nullptr, false, kOnScreen | Trait::ScreenData},
        {{"menu", "select"}, "Select a menu item by its text path", "MENU", true, nullptr, false, kOnScreenLeased},
        {{"element", "get"}, "Read field value", "ELEMENT", true, nullptr, false, kOnScreenLeased | Trait::ScreenData},
        {{"element", "click"}, "Click UI element", "ELEMENT", true, nullptr, false, kOnScreenLeased},
        {{"element", "fill"}, "Fill text field", "ELEMENT", true, nullptr, false, kOnScreenLeased},
        {{"element", "f4"}, "Press F4 to open search help dialog for a field", "ELEMENT", true, nullptr, false, kOnScreenLeased},
        {{"key", "send"}, "Send a key (enter, f1..f12, shift+f1..shift+f12 or a raw VKey number) to a window", "KEY", true, nullptr, false, kOnScreenLeased},
        {{"popup", "close"}, "Close the active popup (sends F12 to wnd[N>0]) and verify it closed", "POPUP", true, nullptr, false, kOnScreenLeased},
        {{"transaction", "start"}, "Execute SAP transaction", "TRANSACTION", true, nullptr, false, Trait::WorkerBound | Trait::ScreenBound | Trait::LeaseCapable},
        {{"credentials", "list"}, "List stored credentials (never shows passwords)", "CREDENTIALS", true, nullptr, false, kListing},
        {{"credentials", "set"}, "Store a credential (the password is prompted for or read from stdin)", "CREDENTIALS", false, nullptr},
        {{"credentials", "delete"}, "Delete a stored credential", "CREDENTIALS", false, nullptr},
        {{"credentials", "import-env"}, "Import a colon-separated credential file into the store", "CREDENTIALS", false, nullptr},
        {{"link", "pair"}, "Pair this desktop with the fairyfly cloud gateway (QR code + number)", "LINK", false, nullptr},
        {{"link", "status"}, "Show the cloud pairing of this desktop", "LINK", false, nullptr},
        {{"link", "run"}, "Serve the cloud gateway's tool calls through fairyfly's dispatcher", "LINK", false, nullptr},
        {{"link", "pause"}, "Disconnect the cloud link until resumed", "LINK", false, nullptr},
        {{"link", "resume"}, "Let a paused cloud link connect again", "LINK", false, nullptr},
        {{"link", "unpair"}, "Forget the cloud pairing on this desktop", "LINK", false, nullptr},
        {{"update", "check"}, "Check whether a newer fairyfly release exists", "UPDATE", false, nullptr},
        {{"update", "apply"}, "Download, verify and install the latest fairyfly release", "UPDATE", false, nullptr},
        {{"mcp"}, "Start the MCP server (stdio)", "MCP SERVER", false, nullptr},
        {{"mcp", "tools"}, "Print the MCP tool table", "MCP SERVER", false, nullptr},
        {{"doctor"}, "Run comprehensive preflight environment and scripting diagnostics", "SYSTEM", true, "system", false, Trait::GlobalControl | Trait::Targetless, Render::Doctor},
        {{"batch"}, "Run many commands from stdin (or --file), one per line, in one process", "SYSTEM", true, "batch", false, static_cast<unsigned>(Trait::Targetless)},
    };
    return table;
}

std::vector<CommandSpec> build_commands() {
    std::vector<CommandSpec> out;
    for (const auto& row : rows()) {
        CommandSpec spec;
        spec.path = row.path;
        spec.summary = row.summary;
        spec.help_group = row.help_group;
        spec.mcp_only = row.mcp_only;
        spec.traits = row.traits;
        spec.render = row.render;
        if (row.tool) {
            spec.tool_name = derived_tool_name(row.path);
            spec.family = row.family_override ? row.family_override : row.path.front();
        }
        out.push_back(std::move(spec));
    }
    return out;
}

} // namespace

std::string CommandSpec::path_string() const { return join(path, " "); }

bool tool_has(const std::string& tool_name, Trait trait) {
    const CommandSpec* spec = find_by_tool(tool_name);
    return spec && spec->has(trait);
}

bool path_has(const std::vector<std::string>& path, Trait trait) {
    const CommandSpec* spec = find_by_path(path);
    return spec && spec->has(trait);
}

Render render_of_tool(const std::string& tool_name) {
    const CommandSpec* spec = find_by_tool(tool_name);
    return spec ? spec->render : Render::Generic;
}

Render render_of_path(const std::vector<std::string>& path) {
    const CommandSpec* spec = find_by_path(path);
    return spec ? spec->render : Render::Generic;
}

std::string derived_tool_name(const std::vector<std::string>& path) {
    std::string name = "gui";
    for (const auto& part : path) {
        name += '_';
        for (char c : part) name += (c == '-') ? '_' : c;
    }
    return name;
}

const std::vector<CommandSpec>& all_commands() {
    static const std::vector<CommandSpec> commands = build_commands();
    return commands;
}

const std::vector<GroupSpec>& groups() {
    static const std::vector<GroupSpec> table = {
        {"session", "SESSION", "Open, attach to, log into and end SAP GUI sessions"},
        {"connection", "CONNECTION", "Saved fairyfly connections"},
        {"screen", "SCREEN", "Read, search and capture the current screen"},
        {"menu", "MENU", "List and select menu bar items"},
        {"element", "ELEMENT", "Read, click and fill screen elements"},
        {"key", "KEY", "Send keys to a window"},
        {"popup", "POPUP", "Handle popup windows"},
        {"transaction", "TRANSACTION", "Start SAP transactions"},
        {"credentials", "CREDENTIALS", "Manage SAP logon credentials in the Windows Credential Manager"},
        {"link", "LINK", "Connect this desktop to the fairyfly cloud gateway (outbound only)"},
        {"update", "UPDATE", "Check for and install new fairyfly releases"},
        {"mcp", "MCP SERVER", "Model Context Protocol server (stdio) and its tool table"},
    };
    return table;
}

std::vector<std::string> help_sections() {
    std::vector<std::string> out;
    for (const auto& group : groups()) out.push_back(group.help_group);
    out.push_back("SYSTEM");
    return out;
}

const CommandSpec* find_by_path(const std::vector<std::string>& path) {
    for (const auto& spec : all_commands())
        if (spec.path == path) return &spec;
    return nullptr;
}

const CommandSpec* find_by_tool(const std::string& tool_name) {
    if (tool_name.empty()) return nullptr;
    for (const auto& spec : all_commands())
        if (spec.tool_name == tool_name) return &spec;
    return nullptr;
}

const GroupSpec* find_group(const std::string& noun) {
    for (const auto& group : groups())
        if (group.noun == noun) return &group;
    return nullptr;
}

std::vector<std::string> families() {
    std::vector<std::string> out;
    for (const auto& spec : all_commands()) {
        if (!spec.is_tool()) continue;
        if (std::find(out.begin(), out.end(), spec.family) == out.end()) out.push_back(spec.family);
    }
    return out;
}

std::string command_of_argv(const std::vector<std::string>& argv) {
    if (argv.empty()) return {};
    for (size_t take = std::min<size_t>(argv.size(), 2); take >= 1; --take) {
        const std::vector<std::string> prefix(argv.begin(), argv.begin() + static_cast<std::ptrdiff_t>(take));
        bool option_free = true;
        for (const auto& token : prefix)
            if (token.empty() || token[0] == '-') option_free = false;
        if (option_free)
            if (const CommandSpec* spec = find_by_path(prefix)) return spec->path_string();
    }
    return argv[0];
}

std::vector<std::string> parse_family_list(const std::string& text) {
    std::vector<std::string> out;
    std::string current;
    auto flush = [&] {
        if (!current.empty() && std::find(out.begin(), out.end(), current) == out.end()) out.push_back(current);
        current.clear();
    };
    for (char c : text) {
        if (c == ',' || std::isspace(static_cast<unsigned char>(c))) flush();
        else current += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    flush();
    return out;
}

std::vector<std::string> unknown_families(const std::vector<std::string>& wanted) {
    const auto known = families();
    std::vector<std::string> out;
    for (const auto& family : wanted)
        if (std::find(known.begin(), known.end(), utils::lower_ascii(family)) == known.end()) out.push_back(family);
    return out;
}

} // namespace fairyfly::command_table
