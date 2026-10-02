#include "include/auth/scopes.h"

#include <algorithm>
#include <cctype>

#include "include/command_table.h"

namespace fairyfly::auth {

namespace {

std::string join(const std::vector<std::string>& parts, const char* sep) {
    std::string out;
    for (const auto& part : parts) out += (out.empty() ? "" : sep) + part;
    return out;
}

} // namespace

std::string normalize_scope(std::string scope) {
    std::transform(scope.begin(), scope.end(), scope.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return scope;
}

std::string verb_scope_of_tool(const std::string& tool_name) {
    const auto* spec = command_table::find_by_tool(tool_name);
    if (!spec || spec->path.size() != 2) return {};
    return spec->family + "." + spec->path.back();
}

std::vector<std::string> verb_scopes() {
    std::vector<std::string> out;
    for (const auto& spec : command_table::all_commands()) {
        if (!spec.is_tool() || spec.path.size() != 2) continue;
        out.push_back(spec.family + "." + spec.path.back());
    }
    return out;
}

std::vector<std::string> verbs_of_family(const std::string& family) {
    std::vector<std::string> out;
    for (const auto& spec : command_table::all_commands())
        if (spec.is_tool() && spec.path.size() == 2 && spec.family == family) out.push_back(spec.path.back());
    return out;
}

std::optional<ScopeError> validate_scope(const std::string& scope) {
    if (scope == "*") return std::nullopt;
    const auto families = command_table::families();
    const auto dot = scope.find('.');
    const std::string family = scope.substr(0, dot);
    if (std::find(families.begin(), families.end(), family) == families.end())
        return ScopeError{"UNKNOWN_FAMILY", "unknown scope '" + scope + "' (families: " + join(families, ", ") +
                                                ", or *; a single tool is <family>.<verb>)"};
    if (dot == std::string::npos) return std::nullopt;
    const auto verbs = verbs_of_family(family);
    const std::string verb = scope.substr(dot + 1);
    if (std::find(verbs.begin(), verbs.end(), verb) != verbs.end()) return std::nullopt;
    if (verbs.empty())
        return ScopeError{"UNKNOWN_SCOPE", "unknown scope '" + scope + "': the family '" + family +
                                               "' has no verb scopes (use '" + family + "')"};
    return ScopeError{"UNKNOWN_SCOPE", "unknown scope '" + scope + "': valid verbs of '" + family + "' are " + join(verbs, ", ")};
}

bool scopes_allow(const std::set<std::string>& granted, bool all, const std::string& family, const std::string& tool_name) {
    if (all) return true;
    if (!family.empty() && granted.count(family) > 0) return true;
    const std::string verb = verb_scope_of_tool(tool_name);
    return !verb.empty() && granted.count(verb) > 0;
}

std::string missing_scope_hint(const std::string& family, const std::string& tool_name) {
    const std::string verb = verb_scope_of_tool(tool_name);
    if (verb.empty()) return "'" + family + "'";
    return "'" + verb + "' (or '" + family + "')";
}

std::string scope_table_markdown() {
    std::string out = "| Family scope | Verb scopes (one tool each) |\n|---|---|\n";
    for (const auto& family : command_table::families()) {
        std::string verbs;
        for (const auto& verb : verbs_of_family(family)) verbs += std::string(verbs.empty() ? "" : ", ") + "`" + family + "." + verb + "`";
        out += "| `" + family + "` | " + (verbs.empty() ? std::string("none (family scope only)") : verbs) + " |\n";
    }
    return out;
}

std::string scope_help_text() {
    std::string out = "Scopes, comma separated: a tool family (" + join(command_table::families(), ", ") +
                      "), a single tool as <family>.<verb> (" + join(verb_scopes(), ", ") +
                      "), or * for all (default: session.list,session.attach,connection.list,screen)";
    return out;
}

} // namespace fairyfly::auth
