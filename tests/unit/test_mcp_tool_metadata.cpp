// Metadata coverage: every MCP tool of the catalog must carry annotations, a family, a docs row, a scope decision
// and correct read-only visibility. A newly added tool that forgets one of these fails here, loudly.
#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>

#include "include/auth/authorize.h"
#include "include/auth/scopes.h"
#include "include/command_table.h"
#include "include/mcp/dispatcher.h"
#include "include/mcp/tool_catalog.h"

using namespace fairyfly;
using namespace fairyfly::mcp;

namespace {

std::string read_docs_mcp() {
    const std::filesystem::path doc = std::filesystem::path(__FILE__).parent_path() / ".." / ".." / "docs" / "MCP.md";
    std::ifstream in(doc, std::ios::binary);
    REQUIRE(in.good());
    std::stringstream buffer;
    buffer << in.rdbuf();
    std::string text = buffer.str();
    text.erase(std::remove(text.begin(), text.end(), '\r'), text.end());
    return text;
}

json sample_args(const std::string& tool) {
    if (tool == "gui_element_get" || tool == "gui_element_click" || tool == "gui_element_f4") return {{"element", "wnd[0]/usr/txtA"}};
    if (tool == "gui_element_fill") return {{"element", "wnd[0]/usr/txtA"}, {"value", "1"}};
    if (tool == "gui_transaction_start") return {{"code", "SE16"}};
    if (tool == "gui_key_send") return {{"key", "enter"}};
    return json::object();
}

Principal scoped(std::set<std::string> scopes) {
    Principal p;
    p.name = "meta";
    p.all_scopes = false;
    p.scopes = std::move(scopes);
    p.authenticated = true;
    p.read_only = false;
    return p;
}

} // namespace

TEST_CASE("tool metadata: catalog is non-empty and names are unique", "[mcp][metadata]") {
    const auto specs = all_tool_specs();
    REQUIRE_FALSE(specs.empty());
    std::set<std::string> names;
    for (const auto& spec : specs) {
        INFO(spec.def.name);
        CHECK(spec.def.name.rfind("gui_", 0) == 0);
        CHECK(names.insert(spec.def.name).second);
        CHECK_FALSE(spec.def.title.empty());
        CHECK_FALSE(spec.def.description.empty());
        CHECK(spec.def.input_schema.is_object());
        CHECK(static_cast<bool>(spec.build_argv));
    }
}

TEST_CASE("tool metadata: annotations are consistent with write_tool", "[mcp][metadata]") {
    for (const auto& spec : all_tool_specs()) {
        INFO("tool " << spec.def.name << ": add annotations via annotations()/tool_catalog_write.cpp");
        REQUIRE(spec.def.annotations.is_object());
        const auto& a = spec.def.annotations;
        REQUIRE(a.contains("readOnlyHint"));
        REQUIRE(a.contains("destructiveHint"));
        REQUIRE(a["readOnlyHint"].is_boolean());
        REQUIRE(a["destructiveHint"].is_boolean());
        const bool ro_hint = a["readOnlyHint"].get<bool>();
        const bool destructive = a["destructiveHint"].get<bool>();
        if (spec.write_tool) CHECK_FALSE(ro_hint);       // hidden in read-only mode, so never advertised as read-only
        if (ro_hint) {
            CHECK_FALSE(spec.write_tool);
            CHECK_FALSE(destructive);
        }
        if (!spec.write_tool && !ro_hint) {
            // "Guarded" tools stay visible in read-only mode (policy refuses only their state-changing arguments),
            // but advertise themselves as not read-only. The set is explicit: a NEW tool must either be a read tool
            // (readOnlyHint true) or a write tool (write_tool), or be added here on purpose.
            static const std::set<std::string> guarded = {
                "gui_batch",          "gui_connection_list", "gui_element_click",   "gui_element_f4",
                "gui_key_send",       "gui_menu_select",     "gui_popup_close",     "gui_session_attach",
                "gui_session_disconnect", "gui_session_launch", "gui_session_login", "gui_transaction_start"};
            INFO(spec.def.name << " is neither read-only nor write_tool: mark it write_tool or add it to the guarded list");
            CHECK(guarded.count(spec.def.name) == 1);
        }
        if (spec.write_tool) CHECK(destructive);  // every write tool warns clients
        CHECK(a.value("title", std::string()) != "");
    }
}

TEST_CASE("tool metadata: every tool has a known family", "[mcp][metadata]") {
    const auto families = command_table::families();
    REQUIRE_FALSE(families.empty());
    for (const auto& spec : all_tool_specs()) {
        INFO("tool " << spec.def.name << " family '" << spec.family << "'");
        REQUIRE_FALSE(spec.family.empty());
        CHECK(std::find(families.begin(), families.end(), spec.family) != families.end());
        // the command table entry of the tool agrees with the catalog
        const auto* command = command_table::find_by_tool(spec.def.name);
        REQUIRE(command != nullptr);
    }
}

TEST_CASE("tool metadata: every tool appears in the docs/MCP.md table", "[mcp][metadata][docs]") {
    const std::string text = read_docs_mcp();
    for (const auto& spec : all_tool_specs()) {
        INFO("tool " << spec.def.name << " missing from docs/MCP.md; regenerate the table with `fairyfly mcp tools --markdown`");
        const std::string row_start = "| `" + spec.def.name + "` | " + spec.family + " |";
        const auto pos = text.find(row_start);
        REQUIRE(pos != std::string::npos);
        const auto eol = text.find('\n', pos);
        const std::string row = text.substr(pos, eol - pos);
        CHECK(row.find(spec.write_tool ? "| yes |" : "| no |") != std::string::npos);
    }
}

TEST_CASE("tool metadata: authorize_call has a scope decision for every tool", "[mcp][metadata][auth]") {
    const auto families = command_table::families();
    Policy rw;
    rw.read_only = false;
    for (const auto& spec : all_tool_specs()) {
        INFO("tool " << spec.def.name);
        const json args = sample_args(spec.def.name);
        // scope = its family -> allowed (batch checks its items, which are empty here)
        const Principal own = scoped({spec.def.name == "gui_session_lease" ? "session.lease" : spec.family});
        const auto allowed = auth::authorize_call(own, spec, spec.family, args, rw, std::nullopt, std::nullopt);
        CHECK(allowed.allowed);
        // another family only -> SCOPE_DENIED (gui_batch is checked per item, so it may pass with an empty list)
        for (const auto& other : families) {
            if (other == spec.family) continue;
            const Principal foreign = scoped({other});
            const auto denied = auth::authorize_call(foreign, spec, spec.family, args, rw, std::nullopt, std::nullopt);
            CHECK_FALSE(denied.allowed);
            CHECK(denied.code == "SCOPE_DENIED");
            break;
        }
        // no scope at all -> denied
        const auto none = auth::authorize_call(scoped({}), spec, spec.family, args, rw, std::nullopt, std::nullopt);
        CHECK_FALSE(none.allowed);
        CHECK(none.code == "SCOPE_DENIED");
        // visibility helper agrees
        CHECK(auth::tool_allowed_for(own, spec));
        CHECK_FALSE(auth::tool_allowed_for(scoped({}), spec));
    }
}

TEST_CASE("tool metadata: read-only principals and read-only lists never expose write tools", "[mcp][metadata]") {
    const auto specs = all_tool_specs();
    std::set<std::string> write_names, read_names;
    for (const auto& spec : specs) (spec.write_tool ? write_names : read_names).insert(spec.def.name);
    REQUIRE_FALSE(write_names.empty());
    REQUIRE_FALSE(read_names.empty());

    Policy ro;
    ro.read_only = true;
    CommandDispatcher read_only([](const std::vector<std::string>&) { return Result{}; }, ro);
    std::set<std::string> listed;
    for (const auto& def : read_only.list_tools()) listed.insert(def.name);
    for (const auto& name : write_names) {
        INFO("write tool " << name);
        CHECK(listed.count(name) == 0);
    }
    for (const auto& name : read_names) {
        INFO("read tool " << name);
        CHECK(listed.count(name) == 1);
    }

    Policy rw;
    rw.read_only = false;
    rw.allow_write = true;
    CommandDispatcher write_mode([](const std::vector<std::string>&) { return Result{}; }, rw);
    std::set<std::string> all_listed;
    for (const auto& def : write_mode.list_tools()) all_listed.insert(def.name);
    for (const auto& spec : specs) CHECK(all_listed.count(spec.def.name) == 1);

    // a read-only token in write mode also loses the write tools
    Principal token_ro;
    token_ro.all_scopes = true;
    token_ro.read_only = true;
    std::set<std::string> token_listed;
    for (const auto& def : write_mode.list_tools_for(token_ro)) token_listed.insert(def.name);
    for (const auto& name : write_names) CHECK(token_listed.count(name) == 0);
    for (const auto& name : read_names) CHECK(token_listed.count(name) == 1);
}

TEST_CASE("tool metadata: every tool resolves to a valid verb scope (doctor and batch are family-only)", "[mcp][metadata][auth]") {
    const auto verbs = auth::verb_scopes();
    for (const auto& spec : all_tool_specs()) {
        INFO("tool " << spec.def.name);
        const std::string verb = auth::verb_scope_of_tool(spec.def.name);
        if (spec.def.name == "gui_doctor" || spec.def.name == "gui_batch") {
            CHECK(verb.empty());
            CHECK_FALSE(auth::validate_scope(spec.family));
            continue;
        }
        REQUIRE_FALSE(verb.empty());
        CHECK_FALSE(auth::validate_scope(verb));
        CHECK(std::find(verbs.begin(), verbs.end(), verb) != verbs.end());
        // a principal holding only that verb scope may use this tool
        CHECK(auth::tool_allowed_for(scoped({verb}), spec));
    }
}

TEST_CASE("tool metadata: docs/MCP.md scope table is generated from the command table", "[mcp][metadata][docs][auth]") {
    const std::string text = read_docs_mcp();
    const std::string begin_marker = "<!-- scope-table:begin -->\n";
    const std::string end_marker = "<!-- scope-table:end -->";
    const auto begin = text.find(begin_marker);
    const auto end = text.find(end_marker);
    REQUIRE(begin != std::string::npos);
    REQUIRE(end != std::string::npos);
    CHECK(text.substr(begin + begin_marker.size(), end - begin - begin_marker.size()) == auth::scope_table_markdown());
}
