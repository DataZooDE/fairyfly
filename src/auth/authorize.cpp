#include "include/auth/authorize.h"

#include <algorithm>
#include <cctype>
#include <cstring>

#include "include/mcp/policy.h"
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
                              const std::optional<std::string>& current_system, bool check_system, const SpecLookup& lookup,
                              int depth) {
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

    if (check_system && !principal.sap_systems.empty() && !system_exempt(family, tool)) {
        if (!current_system || current_system->empty())
            return refuse("SYSTEM_UNKNOWN", "the target SAP system is not known yet and the token is limited to specific systems; "
                                            "attach a session first (gui_session_attach)");
        if (!system_allowed(principal.sap_systems, *current_system))
            return refuse("SYSTEM_DENIED", "token '" + principal.name + "' is not allowed to use SAP system " + *current_system);
    }

    if (!principal.tcodes.empty()) {
        if (tool == "gui_transaction_start") {
            const std::string raw = args.is_object() && args.contains("code") && args["code"].is_string()
                                        ? args["code"].get<std::string>() : std::string();
            const std::string code = normalize_tcode(raw);
            // "/n" alone (back to the start screen) names no transaction.
            const bool bare_prefix = code.empty();
            if (!bare_prefix && !matches_any(principal.tcodes, code))
                return refuse("TCODE_DENIED", "token '" + principal.name + "' is not allowed to run transaction " + code);
        } else if (tool == "gui_element_fill") {
            const std::string element = args.is_object() && args.contains("element") && args["element"].is_string()
                                            ? args["element"].get<std::string>() : std::string();
            if (is_okcd_element(element))
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
                                           false, lookup, depth + 1);
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

bool is_okcd_element(const std::string& element_id) {
    const std::string id = lower(element_id);
    return id == "okcd" || (id.size() >= 5 && id.compare(id.size() - 5, 5, "/okcd") == 0);
}

mcp::PolicyDecision authorize_call(const mcp::Principal& principal, const mcp::ToolSpec& spec, const std::string& family,
                                   const mcp::json& args, const mcp::Policy& server_policy,
                                   std::optional<std::string> current_system, std::optional<std::string> current_tcode,
                                   const SpecLookup& lookup) {
    (void)current_tcode;
    return authorize_impl(principal, spec, family, args, server_policy, current_system, true, lookup, 0);
}

bool tool_allowed_for(const mcp::Principal& principal, const mcp::ToolSpec& spec) {
    if (!has_scope(principal, spec.family)) return false;
    if (principal.read_only && spec.write_tool) return false;
    return true;
}

} // namespace fairyfly::auth
