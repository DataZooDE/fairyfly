#include "include/auth/authorize.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "include/mcp/policy.h"
#include "include/vkey.h"
#include "include/mcp/tool_catalog.h"

namespace fairyfly::auth {

namespace {

using mcp::json;
using mcp::PolicyDecision;

char lc(char c) { return static_cast<char>(std::tolower(static_cast<unsigned char>(c))); }

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

PolicyDecision refuse(const char* code, std::string message) {
    PolicyDecision d;
    d.allowed = false;
    d.code = code;
    d.message = std::move(message);
    return d;
}

bool has_scope(const mcp::Principal& p, const std::string& family) {
    return p.all_scopes || (!family.empty() && p.scopes.count(family) > 0);
}

bool system_exempt(const std::string& family, const std::string& tool) {
    return family == "session" || family == "connection" || family == "system" || family == "credentials" ||
           tool == "gui_batch";
}

bool matches_any(const std::vector<std::string>& patterns, const std::string& text) {
    return std::any_of(patterns.begin(), patterns.end(), [&](const std::string& p) { return glob_match(p, text); });
}

bool system_allowed(const std::vector<std::string>& patterns, const std::string& current) {
    const auto slash = current.find('/');
    const std::string sid = slash == std::string::npos ? current : current.substr(0, slash);
    for (const auto& pattern : patterns) {
        if (glob_match(pattern, current)) return true;
        if (pattern.find('/') == std::string::npos && glob_match(pattern, sid)) return true;  // "A4H" = any client
    }
    return false;
}

PolicyDecision authorize_impl(const mcp::Principal& principal, const mcp::ToolSpec& spec, const std::string& family,
                              const json& args, const mcp::Policy& server_policy,
                              const std::optional<std::string>& current_system,
                              const std::optional<std::string>& current_tcode, bool check_system,
                              const SpecLookup& lookup, int depth) {
    const std::string& tool = spec.def.name;

    if (!has_scope(principal, family))
        return refuse("SCOPE_DENIED", "token '" + principal.name + "' has no access to the '" + family + "' tool family");

    if (principal.read_only || server_policy.read_only) {
        mcp::Policy narrowed = server_policy;
        narrowed.read_only = true;
        const auto decision = mcp::check_call(spec, args, narrowed, [](const char*) { return std::string(); });
        if (!decision.allowed)
            return refuse("READ_ONLY", principal.read_only
                                           ? "token '" + principal.name + "' is read-only: " + tool + " changes SAP state"
                                           : "the server runs read-only: " + tool + " changes SAP state");
    }

    // cleanup deletes saved connection files whatever their name: never for a token limited to named connections.
    if (!principal.connections.empty() && tool == "gui_connection_list" && args.is_object() && args.contains("cleanup") &&
        !(args["cleanup"].is_boolean() && !args["cleanup"].get<bool>()))
        return refuse("CONNECTION_DENIED", "token '" + principal.name + "' is limited to specific saved connections; "
                                           "gui_connection_list with cleanup=true could delete other saved connections and is refused");

    // gui_doctor reports global connection and session counts of the desktop, which name other connections' existence.
    if (!principal.connections.empty() && tool == "gui_doctor")
        return refuse("CONNECTION_DENIED", "token '" + principal.name + "' is limited to specific saved connections; gui_doctor "
                                           "reports environment-wide connection and session counts and is refused");

    if (check_system && !principal.sap_systems.empty() && !system_exempt(family, tool)) {
        if (!current_system || current_system->empty())
            return refuse("SYSTEM_UNKNOWN", "the target SAP system is not known yet and the token is limited to specific systems; "
                                            "attach a session first (gui_session_attach)");
        if (!system_allowed(principal.sap_systems, *current_system))
            return refuse("SYSTEM_DENIED", "token '" + principal.name + "' is not allowed to use SAP system " + *current_system);
    }

    if (!principal.tcodes.empty()) {
        // Screen-acting tools run inside the transaction that is open NOW: it must be allowlisted too.
        // Fails closed when it is unknown (residual race: it can change during the call, see docs/MCP.md).
        if (check_system && acts_on_screen(family)) {
            const std::string open = current_tcode ? normalize_tcode(*current_tcode) : std::string();
            if (open.empty())
                return refuse("TCODE_DENIED", "the open SAP transaction is not known and the token is limited to specific "
                                              "transactions; start an allowed one with gui_transaction_start");
            if (!matches_any(principal.tcodes, open))
                return refuse("TCODE_DENIED", "token '" + principal.name + "' is not allowed to act in transaction " + open);
        }
        if (tool == "gui_transaction_start") {
            const std::string raw = args.is_object() && args.contains("code") && args["code"].is_string()
                                        ? args["code"].get<std::string>() : std::string();
            const std::string code = normalize_tcode(raw);
            // "/n" alone (back to the start screen) names no transaction.
            const bool bare_prefix = code.empty();
            if (!bare_prefix && !matches_any(principal.tcodes, code))
                return refuse("TCODE_DENIED", "token '" + principal.name + "' is not allowed to run transaction " + code);
        } else if (!principal.allow_navigation && tool == "gui_menu_select") {
            return refuse("TCODE_DENIED", "gui_menu_select is denied for tokens with a T-code allowlist because menu paths can "
                                          "start other transactions; use gui_transaction_start, or ask the operator to create "
                                          "the token with --allow-navigation");
        } else if (!principal.allow_navigation && tool == "gui_key_send") {
            const std::string key = args.is_object() && args.contains("key") && args["key"].is_string()
                                        ? args["key"].get<std::string>() : std::string();
            if (!sap::parse_vkey(key))
                return refuse("INVALID_ARGUMENT", "unknown key '" + key + "'; supported key names: " + sap::supported_key_names_text());
            if (!tcode_safe_key(key))
                return refuse("TCODE_DENIED", "key '" + key + "' can leave the transaction and is denied for tokens with a T-code "
                                              "allowlist. Allowed keys: " + tcode_safe_key_spellings() + "; ask the operator to "
                                              "create the token with --allow-navigation");
        } else if (!principal.allow_navigation && tool == "gui_popup_close") {
            // The popup's own default (F12 = cancel, only ever sent to the popup window) stays usable; any other VKey
            // must be a navigation-safe one (Shift+F3 = 15 would exit the transaction).
            if (args.is_object() && args.contains("vkey")) {
                const auto& v = args["vkey"];
                const bool safe = v.is_number_integer() &&
                                  (v.get<long long>() == 12 || tcode_safe_key(std::to_string(v.get<long long>())));
                if (!safe)
                    return refuse("TCODE_DENIED", "gui_popup_close vkey " + v.dump() + " can leave the transaction and is denied for "
                                                  "tokens with a T-code allowlist (allowed: 12 (default), 0, 4, 8, 80-83, see gui_key_send for the names); ask the "
                                                  "operator to create the token with --allow-navigation");
            }
        } else if (tool == "gui_element_fill") {
            // `element` and its aliases `id` / `element_id`: all of them are checked, whichever the builder will use.
            bool okcd = false;
            for (const char* key : {"element", "element_id", "id"})
                if (args.is_object() && args.contains(key) && args[key].is_string() && is_okcd_element(args[key].get<std::string>()))
                    okcd = true;
            if (okcd)
                return refuse("TCODE_DENIED", "typing into the command field is blocked for tokens with a T-code allowlist; "
                                              "use gui_transaction_start");
        }
    }

    if (tool == "gui_batch" && depth == 0 && args.is_object() && args.contains("items") && args["items"].is_array()) {
        std::size_t index = 0;
        for (const auto& item : args["items"]) {
            ++index;
            if (!item.is_object() || !item.contains("tool") || !item["tool"].is_string()) continue;  // shape errors: the dispatcher reports them
            const std::string name = item["tool"].get<std::string>();
            const mcp::ToolSpec* item_spec = nullptr;
            if (lookup) item_spec = lookup(name);
            else {
                static const std::vector<mcp::ToolSpec> catalog = mcp::all_tool_specs();
                for (const auto& s : catalog)
                    if (s.def.name == name) { item_spec = &s; break; }
            }
            if (!item_spec || name == "gui_batch") continue;  // unknown / nested: rejected by the dispatcher
            const json item_args = item.contains("arguments") ? item["arguments"] : json::object();
            auto decision = authorize_impl(principal, *item_spec, item_spec->family, item_args, server_policy, current_system,
                                           current_tcode, false, lookup, depth + 1);
            if (!decision.allowed) {
                decision.message = "batch item " + std::to_string(index) + " (" + name + "): " + decision.message;
                return decision;
            }
        }
    }
    return PolicyDecision{};
}

} // namespace

bool glob_match(const std::string& pattern, const std::string& text) {
    std::size_t p = 0, t = 0, star = std::string::npos, mark = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || lc(pattern[p]) == lc(text[t]))) {
            ++p; ++t;
        } else if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            mark = t;
        } else if (star != std::string::npos) {
            p = star + 1;
            t = ++mark;
        } else {
            return false;
        }
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

std::string normalize_tcode(const std::string& raw) {
    std::string s = raw;
    const auto first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    s = s.substr(first);
    if (s.size() >= 2 && s[0] == '/' && (s[1] == 'n' || s[1] == 'N' || s[1] == 'o' || s[1] == 'O' || s[1] == '*')) {
        s = s.substr(2);
        const auto next = s.find_first_not_of(" \t");
        s = next == std::string::npos ? std::string() : s.substr(next);
    }
    const auto end = s.find_first_of(" \t\r\n;");
    if (end != std::string::npos) s = s.substr(0, end);
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
    return s;
}

bool acts_on_screen(const std::string& family) {
    return family == "screen" || family == "element" || family == "key" || family == "popup" || family == "menu";
}

std::string tcode_safe_key_spellings() {
    return "enter, f4, f8, pageup (page_up, pgup), pagedown (page_down, pgdn), pagetop (ctrl+pageup), "
           "pagebottom (ctrl+pagedown), or the raw VKeys 0, 4, 8, 80, 81, 82, 83 (case, blanks, '_' and '-' are ignored)";
}

bool tcode_safe_key(const std::string& key) {
    const auto vkey = sap::parse_vkey(key);
    if (!vkey) return false;
    switch (*vkey) {
    case 0:   // Enter
    case 4:   // F4 value help
    case 8:   // F8 execute
    case 80: case 81: case 82: case 83:  // page keys
        return true;
    default:
        return false;
    }
}

bool is_okcd_element(const std::string& element_id) {
    const std::string id = lower(element_id);
    return id == "okcd" || (id.size() >= 5 && id.compare(id.size() - 5, 5, "/okcd") == 0);
}

mcp::PolicyDecision authorize_call(const mcp::Principal& principal, const mcp::ToolSpec& spec, const std::string& family,
                                   const mcp::json& args, const mcp::Policy& server_policy,
                                   std::optional<std::string> current_system, std::optional<std::string> current_tcode,
                                   const SpecLookup& lookup) {
    return authorize_impl(principal, spec, family, args, server_policy, current_system, current_tcode, true, lookup, 0);
}

namespace {

bool session_system_tool(const std::string& tool, const mcp::json& args) {
    if (tool == "gui_session_launch" || tool == "gui_session_login" || tool == "gui_session_attach") return true;
    return tool == "gui_session_disconnect" && args.is_object() && args.contains("close_session") &&
           args["close_session"].is_boolean() && args["close_session"].get<bool>();
}

bool targetless_tool(const std::string& tool) {
    return tool == "gui_session_list" || tool == "gui_connection_list" || tool == "gui_credentials_list" ||
           tool == "gui_doctor" || tool == "gui_batch";
}

} // namespace

bool needs_session_target(const mcp::Principal& principal, const std::string& tool, const mcp::json& args) {
    if (!principal.sap_systems.empty() && session_system_tool(tool, args)) return true;
    return !principal.connections.empty() && !targetless_tool(tool);
}

mcp::PolicyDecision authorize_session_target(const mcp::Principal& principal, const std::string& tool, const mcp::json& args,
                                             const SessionTarget& target) {
    if (!principal.sap_systems.empty() && tool == "gui_session_launch") {
        // The system of a SAP Logon entry is unknown before it is opened. Facts of other sessions are not proof, so the
        // operator has to vouch for the entry name with --connections (fail closed otherwise).
        const std::string name = args.is_object() && args.contains("name") && args["name"].is_string()
                                     ? args["name"].get<std::string>() : std::string();
        if (name.empty() || principal.connections.empty() || !matches_any(principal.connections, name))
            return refuse("SYSTEM_UNKNOWN",
                          "token '" + principal.name + "' is limited to specific SAP systems and the system of SAP Logon entry '" +
                              name + "' cannot be known before it is opened; launching is only allowed for entries named by the "
                              "token's --connections list (create the token with --connections " +
                              (name.empty() ? std::string("NAME") : name) + ") or start the session on the desktop and attach");
        if (target.ambiguous)
            return refuse("SYSTEM_UNKNOWN", "open sessions of SAP Logon entry '" + name + "' run on different SAP systems; "
                                            "the target cannot be determined");
        if (!target.system.empty() && !system_allowed(principal.sap_systems, target.system))
            return refuse("SYSTEM_DENIED", "token '" + principal.name + "' is not allowed to use SAP system " + target.system);
    } else if (!principal.sap_systems.empty() && session_system_tool(tool, args)) {
        if (target.system.empty())
            return refuse("SYSTEM_UNKNOWN",
                          "token '" + principal.name + "' is limited to specific SAP systems and the system of this " +
                              (tool == "gui_session_launch" ? "SAP Logon entry" : "connection/session") +
                              " cannot be determined without contacting SAP; ask the operator to start it on the desktop "
                              "and attach, or use a token without a system allowlist");
        if (!system_allowed(principal.sap_systems, target.system))
            return refuse("SYSTEM_DENIED", "token '" + principal.name + "' is not allowed to use SAP system " + target.system);
    }
    if (!principal.connections.empty() && !targetless_tool(tool)) {
        const std::string name = tool == "gui_session_launch" && args.is_object() && args.contains("name") && args["name"].is_string()
                                     ? args["name"].get<std::string>() : target.connection_name;
        if (name.empty())
            return refuse("CONNECTION_DENIED", "token '" + principal.name + "' is limited to specific saved connections and the "
                                               "connection of this call cannot be determined");
        if (!matches_any(principal.connections, name))
            return refuse("CONNECTION_DENIED", "token '" + principal.name + "' is not allowed to use connection '" + name + "'");
    }
    return PolicyDecision{};
}

// ---- result filtering for tokens limited to named connections ------------------------------------------
namespace {

using mcp::json;

bool has_only_keys(const json& obj, std::initializer_list<const char*> allowed) {
    for (auto it = obj.begin(); it != obj.end(); ++it) {
        bool known = false;
        for (const char* key : allowed)
            if (it.key() == key) { known = true; break; }
        if (!known) return false;
    }
    return true;
}

std::optional<std::string> string_field(const json& obj, const char* key) {
    if (!obj.is_object() || !obj.contains(key) || !obj[key].is_string()) return std::nullopt;
    std::string value = obj[key].get<std::string>();
    if (value.empty()) return std::nullopt;
    return value;
}

std::size_t array_size(const json& obj, const char* key) {
    return obj.contains(key) && obj[key].is_array() ? obj[key].size() : 0;
}

/// gui_session_list: keep connections whose live description is allowed; recompute every total.
bool filter_session_list(json& data, const std::vector<std::string>& patterns) {
    if (!data.is_object() || !data.contains("connections") || !data["connections"].is_array()) return false;
    if (!has_only_keys(data, {"connections", "total_connections", "total_sessions", "backend_scripting_disabled",
                              "connection_enumeration_errors", "session_enumeration_errors"}))
        return false;
    json kept = json::array();
    long long total_sessions = 0, conn_errors = 0, session_errors = 0;
    bool scripting_disabled = false;
    for (const auto& conn : data["connections"]) {
        if (!conn.is_object()) return false;
        const auto name = string_field(conn, "description");
        if (!name || !matches_any(patterns, *name)) continue;  // dropped: not allowed, or its name cannot be determined
        if (!conn.contains("sessions") || !conn["sessions"].is_array()) return false;
        total_sessions += static_cast<long long>(conn["sessions"].size());
        session_errors += static_cast<long long>(array_size(conn, "session_errors"));
        if (conn.contains("error")) ++conn_errors;
        if (conn.contains("backend_scripting_disabled") && conn["backend_scripting_disabled"] == true) scripting_disabled = true;
        kept.push_back(conn);
    }
    const auto count = static_cast<long long>(kept.size());
    data["connections"] = std::move(kept);
    data["total_connections"] = count;
    data["total_sessions"] = total_sessions;
    data["connection_enumeration_errors"] = conn_errors;
    data["session_enumeration_errors"] = session_errors;
    data["backend_scripting_disabled"] = scripting_disabled;
    return true;
}

/// gui_connection_list / gui_credentials_list: keep rows whose `name_key` matches; recompute `count`.
bool filter_named_rows(json& data, const char* rows_key, const char* name_key, const std::vector<std::string>& patterns) {
    if (!data.is_object() || !data.contains(rows_key) || !data[rows_key].is_array()) return false;
    if (!has_only_keys(data, {rows_key, "count"})) return false;
    json kept = json::array();
    for (const auto& row : data[rows_key]) {
        if (!row.is_object()) return false;
        const auto name = string_field(row, name_key);
        if (!name || !matches_any(patterns, *name)) continue;
        kept.push_back(row);
    }
    const auto count = static_cast<long long>(kept.size());
    data[rows_key] = std::move(kept);
    data["count"] = count;
    return true;
}

} // namespace

bool listing_needs_filter(const mcp::Principal& principal, const std::string& tool) {
    return !principal.connections.empty() &&
           (tool == "gui_session_list" || tool == "gui_connection_list" || tool == "gui_credentials_list");
}

bool filter_listing_for_connections(const mcp::Principal& principal, const std::string& tool, Result& result) {
    if (!listing_needs_filter(principal, tool)) return true;
    result.diagnostics = json();  // never carries anything of the dropped entries
    if (result.status != Result::Status::Success) {
        // Error details of these listings can name other connections/credentials: keep the code only.
        std::string code = "ERROR";
        if (result.error.is_object() && result.error.contains("code") && result.error["code"].is_string())
            code = result.error["code"].get<std::string>();
        result.error = {{"code", code}, {"message", "the listing failed (details are withheld from tokens limited to named connections)"}};
        return true;
    }
    bool ok = false;
    if (tool == "gui_session_list") ok = filter_session_list(result.data, principal.connections);
    else if (tool == "gui_connection_list") ok = filter_named_rows(result.data, "connections", "description", principal.connections);
    else ok = filter_named_rows(result.data, "credentials", "connection", principal.connections);
    if (!ok) {
        result.data = json();  // never return the unfiltered data
        return false;
    }
    return true;
}

int rate_family_limit(const mcp::Principal& principal, const std::string& family) {
    const auto it = principal.rate_families.find(family);
    return it == principal.rate_families.end() || it->second < 1 ? 0 : it->second;
}

bool tool_allowed_for(const mcp::Principal& principal, const mcp::ToolSpec& spec) {
    if (!has_scope(principal, spec.family)) return false;
    if (principal.read_only && spec.write_tool) return false;
    return true;
}

} // namespace fairyfly::auth
