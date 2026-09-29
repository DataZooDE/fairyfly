#include <catch2/catch_test_macros.hpp>

#include <CLI/CLI.hpp>
#include <regex>
#include <set>

#include "include/command_table.h"
#include "include/commands/cli_app.h"
#include "include/commands/command_registry.h"
#include "include/mcp/tool_catalog.h"
#include "include/mcp/types.h"

using namespace fairyfly::mcp;
using Argv = std::vector<std::string>;

namespace {

struct Sample {
    std::string tool;
    json args;
    Argv expected;
};

const ToolSpec& spec_of(const std::vector<ToolSpec>& specs, const std::string& name) {
    for (const auto& s : specs)
        if (s.def.name == name) return s;
    FAIL("tool not in catalog: " << name);
    return specs.front();
}

const std::vector<Sample>& samples() {
    static const std::vector<Sample> table = {
        {"gui_doctor", json::object(), {"doctor"}},
        {"gui_session_list", json::object(), {"session", "list"}},
        {"gui_connection_list", json::object(), {"connection", "list"}},
        {"gui_connection_list", {{"cleanup", true}}, {"connection", "list", "--cleanup"}},
        {"gui_session_attach", {{"session_id", "/app/con[0]/ses[0]"}}, {"session", "attach", "--session-id", "/app/con[0]/ses[0]"}},
        {"gui_session_launch", {{"name", "PRD"}}, {"session", "launch", "PRD"}},
        {"gui_session_launch",
         {{"name", "PRD"}, {"login", true}, {"credential", "c1"}, {"multiple_logon", "keep"}, {"allow_sapshcut", true}},
         {"session", "launch", "PRD", "--login", "--credential", "c1", "--multiple-logon", "keep", "--allow-sapshcut"}},
        {"gui_session_login", json::object(), {"session", "login"}},
        {"gui_session_login", {{"connection", 2}, {"credential", "c"}, {"multiple_logon", "terminate"}},
         {"session", "login", "--connection", "2", "--credential", "c", "--multiple-logon", "terminate"}},
        {"gui_transaction_start", {{"code", "SE38"}}, {"transaction", "start", "SE38"}},
        {"gui_transaction_start", {{"code", "/nSM37"}, {"connection", 3}}, {"transaction", "start", "/nSM37", "--connection", "3"}},
        {"gui_screen_read", json::object(), {"screen", "read", "--max-rows", "20", "--compact", "--output", "markdown"}},
        {"gui_screen_read",
         {{"tab", "tabpTAB2"}, {"only", "buttons"}, {"text_contains", "Save"}, {"id_contains", "btn"},
          {"type", "GuiButton"}, {"first", true}, {"max_rows", 5}, {"skip_trees", true}, {"probe_all", true},
          {"format", "json"}, {"compact", false}, {"connection", 1}},
         {"screen", "read", "--tab", "tabpTAB2", "--only-buttons", "--text-contains", "Save", "--id-contains", "btn",
          "--type", "GuiButton", "--first", "--max-rows", "5", "--skip-trees", "--probe-all", "--connection", "1",
          "--output", "json"}},
        {"gui_screen_read", {{"no_tabs", true}, {"only", "editable"}},
         {"screen", "read", "--no-tabs", "--only-editable", "--max-rows", "20", "--compact", "--output", "markdown"}},
        {"gui_screen_read", {{"only", "fields"}, {"text_contains", "-danger"}},
         {"screen", "read", "--only-fields", "--text-contains=-danger", "--max-rows", "20", "--compact", "--output", "markdown"}},
        {"gui_screen_read", {{"only", "f4_fields"}},
         {"screen", "read", "--only-f4-fields", "--max-rows", "20", "--compact", "--output", "markdown"}},
        {"gui_screen_find", {{"name_contains", "Save"}},
         {"screen", "find", "--name-contains", "Save", "--limit", "10", "--output", "markdown"}},
        {"gui_screen_find", {{"id_contains", "btn"}, {"type", "GuiButton"}, {"limit", 3}, {"probe_all", true}, {"connection", 0}},
         {"screen", "find", "--id-contains", "btn", "--type", "GuiButton", "--limit", "3", "--probe-all", "--connection", "0",
          "--output", "markdown"}},
        {"gui_element_get", {{"element", "wnd[0]/usr/txtX"}}, {"element", "get", "wnd[0]/usr/txtX"}},
        {"gui_element_get", {{"element", "/app/con[0]/ses[0]/wnd[0]/usr/cntl"}, {"list_nodes", true}},
         {"element", "get", "/app/con[0]/ses[0]/wnd[0]/usr/cntl", "--list-nodes"}},
        {"gui_menu_list", json::object(), {"menu", "list"}},
        {"gui_menu_list", {{"window", "@active"}}, {"menu", "list", "--window", "@active"}},
        {"gui_screen_capture", json::object(), {"screen", "capture", "--format", "base64"}},
        {"gui_screen_capture", {{"scale", 0.5}, {"x", 1}, {"y", 2}, {"width", 3}, {"height", 4}},
         {"screen", "capture", "--format", "base64", "--scale", "0.5", "--x", "1", "--y", "2", "--width", "3", "--height", "4"}},
        {"gui_screen_capture", {{"scale", 800}}, {"screen", "capture", "--format", "base64", "--scale", "800"}},
        {"gui_credentials_list", json::object(), {"credentials", "list"}},
        {"gui_element_click", {{"element", "wnd[0]/tbar[1]/btn[8]"}}, {"element", "click", "wnd[0]/tbar[1]/btn[8]"}},
        {"gui_element_click",
         {{"element", "wnd[0]/usr/tree"}, {"connection", 1}, {"wait_for_window", true}, {"timeout_ms", 3000}, {"row", 2},
          {"column", "COL"}, {"doubleclick", true}, {"node_key", "K"}, {"tree_action", "select"}, {"menu_item", "M"}},
         {"element", "click", "wnd[0]/usr/tree", "--connection", "1", "--wait-for-window", "--timeout", "3000", "--row", "2",
          "--column", "COL", "--doubleclick", "--node-key", "K", "--tree-action", "select", "--menu-item", "M"}},
        {"gui_key_send", {{"key", "enter"}}, {"key", "send", "enter"}},
        {"gui_key_send", {{"key", "f8"}, {"window", "wnd[1]"}, {"connection", 2}},
         {"key", "send", "f8", "--window", "wnd[1]", "--connection", "2"}},
        {"gui_popup_close", json::object(), {"popup", "close"}},
        {"gui_popup_close", {{"vkey", 0}}, {"popup", "close", "--vkey", "0"}},
        {"gui_element_f4", {{"element", "wnd[0]/usr/ctxtF"}}, {"element", "f4", "wnd[0]/usr/ctxtF"}},
        {"gui_menu_select", {{"path", "Table/Save"}}, {"menu", "select", "Table/Save"}},
        {"gui_menu_select", {{"path", "System/Log off"}, {"window", "@active"}},
         {"menu", "select", "System/Log off", "--window", "@active"}},
        {"gui_session_disconnect", json::object(), {"session", "disconnect"}},
        {"gui_session_disconnect", {{"connection", 4}, {"close_session", true}}, {"session", "disconnect", "--connection", "4", "--close-session"}},
    };
    return table;
}

} // namespace

TEST_CASE("MCP catalog: every tool is well formed", "[mcp][tools]") {
    const auto specs = all_tool_specs();
    REQUIRE(specs.size() >= 20);
    const std::regex valid_name("^[A-Za-z0-9_.-]{1,128}$");
    std::set<std::string> seen;
    for (const auto& spec : specs) {
        INFO(spec.def.name);
        CHECK(std::regex_match(spec.def.name, valid_name));
        CHECK(spec.def.name.rfind("gui_", 0) == 0);
        CHECK_FALSE(spec.family.empty());
        CHECK(seen.insert(spec.def.name).second);
        CHECK_FALSE(spec.def.title.empty());
        CHECK(spec.def.description.size() > 20);
        REQUIRE(spec.def.input_schema.is_object());
        CHECK(spec.def.input_schema["type"] == "object");
        REQUIRE(spec.def.input_schema.contains("additionalProperties"));
        CHECK(spec.def.input_schema["additionalProperties"] == false);
        REQUIRE(spec.def.annotations.is_object());
        for (const char* hint : {"readOnlyHint", "destructiveHint", "idempotentHint", "openWorldHint"}) {
            INFO(hint);
            CHECK(spec.def.annotations.contains(hint));
            CHECK(spec.def.annotations[hint].is_boolean());
        }
        CHECK(spec.def.annotations.contains("title"));
        CHECK(static_cast<bool>(spec.build_argv));
        if (spec.def.annotations.value("readOnlyHint", false)) {
            REQUIRE(spec.def.meta.is_object());
            CHECK(spec.def.meta["anthropic/maxResultSizeChars"] == 60000);
        }
    }
    for (const char* name : {"gui_doctor", "gui_session_list", "gui_connection_list", "gui_session_attach", "gui_session_launch", "gui_session_login",
                             "gui_transaction_start", "gui_screen_read", "gui_screen_find", "gui_element_get", "gui_menu_list", "gui_screen_capture",
                             "gui_credentials_list", "gui_element_click", "gui_key_send", "gui_popup_close", "gui_element_f4",
                             "gui_menu_select", "gui_session_disconnect", "gui_batch"})
        CHECK(seen.count(name) == 1);
}

TEST_CASE("MCP catalog: batch annotations and destructive flags", "[mcp][tools]") {
    const auto specs = read_tool_specs();
    const auto& batch = spec_of(specs, "gui_batch");
    CHECK(batch.def.annotations["destructiveHint"] == true);
    CHECK(batch.def.annotations["readOnlyHint"] == false);
    CHECK(spec_of(specs, "gui_element_click").def.annotations["destructiveHint"] == true);
    CHECK(spec_of(specs, "gui_key_send").def.annotations["destructiveHint"] == true);
    CHECK(spec_of(specs, "gui_menu_select").def.annotations["destructiveHint"] == true);
    CHECK(spec_of(specs, "gui_screen_read").def.annotations["readOnlyHint"] == true);
    // Documented as destructive.
    CHECK(spec_of(specs, "gui_session_launch").def.input_schema["properties"]["multiple_logon"]["enum"].size() == 4);
    CHECK(spec_of(specs, "gui_session_launch").def.description.find("DESTRUCTIVE") != std::string::npos);
    CHECK(spec_of(specs, "gui_session_disconnect").def.description.find("DESTRUCTIVE") != std::string::npos);
    CHECK(spec_of(specs, "gui_screen_read").def.description.find("max_rows") != std::string::npos);
}

TEST_CASE("MCP catalog: no secret-bearing properties, capture never writes files", "[mcp][tools]") {
    const std::regex forbidden("password|passwd|secret|credentials_file|credentials-file|stdin|token|^file$|^show$",
                               std::regex::icase);
    for (const auto& spec : all_tool_specs()) {
        INFO(spec.def.name);
        for (auto it = spec.def.input_schema["properties"].begin(); it != spec.def.input_schema["properties"].end(); ++it)
            CHECK_FALSE(std::regex_search(it.key(), forbidden));
    }
    const auto specs = read_tool_specs();
    const auto& capture = spec_of(specs, "gui_screen_capture");
    for (const auto& args : {json::object(), json{{"scale", 0.3}}, json{{"x", 1}, {"y", 2}, {"width", 3}, {"height", 4}}}) {
        for (const auto& token : capture.build_argv(args, Policy{})) {
            CHECK(token != "--file");
            CHECK(token != "-f");
            CHECK(token != "--show");
        }
    }
    CHECK_THROWS_AS(capture.build_argv(json{{"file", "x.png"}}, Policy{}), std::invalid_argument);
    CHECK_THROWS_AS(capture.build_argv(json{{"show", true}}, Policy{}), std::invalid_argument);
    CHECK_THROWS_AS(spec_of(specs, "gui_session_login").build_argv(json{{"password", "x"}}, Policy{}), std::invalid_argument);
    CHECK_THROWS_AS(spec_of(specs, "gui_session_login").build_argv(json{{"credentials_file", "x"}}, Policy{}), std::invalid_argument);
}

TEST_CASE("MCP catalog: sample arguments produce the exact argv", "[mcp][tools]") {
    const auto specs = read_tool_specs();
    for (const auto& sample : samples()) {
        INFO(sample.tool << " " << sample.args.dump());
        CHECK(spec_of(specs, sample.tool).build_argv(sample.args, Policy{}) == sample.expected);
    }
}

TEST_CASE("MCP catalog: connection precedence argument > policy default", "[mcp][tools]") {
    const auto specs = read_tool_specs();
    Policy policy;
    policy.default_connection = 7;
    CHECK(spec_of(specs, "gui_transaction_start").build_argv(json{{"code", "SE38"}}, policy) ==
          Argv{"transaction", "start", "SE38", "--connection", "7"});
    CHECK(spec_of(specs, "gui_transaction_start").build_argv(json{{"code", "SE38"}, {"connection", 2}}, policy) ==
          Argv{"transaction", "start", "SE38", "--connection", "2"});
    // Tools without session context never get one.
    CHECK(spec_of(specs, "gui_session_list").build_argv(json::object(), policy) == Argv{"session", "list"});
    CHECK(spec_of(specs, "gui_session_attach").build_argv(json{{"session_id", "s"}}, policy) == Argv{"session", "attach", "--session-id", "s"});
    Policy json_policy;
    json_policy.default_format = "json";
    CHECK(spec_of(specs, "gui_screen_read").build_argv(json::object(), json_policy).back() == "json");
    CHECK(spec_of(specs, "gui_screen_read").build_argv(json{{"format", "markdown"}}, json_policy).back() == "markdown");
}

TEST_CASE("MCP catalog: invalid arguments throw std::invalid_argument", "[mcp][tools]") {
    const auto specs = read_tool_specs();
    struct Bad { std::string tool; json args; };
    const std::vector<Bad> bad = {
        {"gui_doctor", {{"x", 1}}},
        {"gui_transaction_start", json::object()},
        {"gui_transaction_start", {{"code", ""}}},
        {"gui_transaction_start", {{"code", "-x"}}},
        {"gui_transaction_start", {{"code", 5}}},
        {"gui_transaction_start", {{"code", "SE38"}, {"connection", "1"}}},
        {"gui_transaction_start", {{"code", "SE38"}, {"connection", -1}}},
        {"gui_transaction_start", {{"code", "SE38"}, {"connection", 1.5}}},
        {"gui_screen_read", {{"max_rows", 0}}},
        {"gui_screen_read", {{"max_rows", 201}}},
        {"gui_screen_read", {{"only", "everything"}}},
        {"gui_screen_read", {{"format", "toon"}}},
        {"gui_screen_read", {{"tab", "t"}, {"no_tabs", true}}},
        {"gui_screen_read", {{"first", "yes"}}},
        {"gui_screen_find", json::object()},
        {"gui_screen_find", {{"name_contains", "a"}, {"limit", 101}}},
        {"gui_screen_find", {{"name_contains", "a"}, {"limit", 0}}},
        {"gui_element_get", json::object()},
        {"gui_element_get", {{"element", "--list-nodes"}}},
        {"gui_element_click", {{"element", "wnd[0]/usr/btn"}, {"tree_action", "explode"}}},
        {"gui_element_click", {{"element", "wnd[0]/usr/btn"}, {"timeout_ms", 1}}},
        {"gui_element_click", {{"element", "wnd[0]/usr/btn"}, {"row", -1}}},
        {"gui_key_send", json::object()},
        {"gui_popup_close", {{"vkey", 100}}},
        {"gui_session_launch", {{"name", "PRD"}, {"multiple_logon", "destroy"}}},
        {"gui_session_launch", json::object()},
        {"gui_session_launch", {{"name", "-PRD"}}},
        {"gui_session_attach", json::object()},
        {"gui_session_attach", {{"session_id", ""}}},
        {"gui_menu_select", json::object()},
        {"gui_screen_capture", {{"scale", 0}}},
        {"gui_screen_capture", {{"width", 0}}},
        {"gui_session_disconnect", {{"close_session", "true"}}},
        {"gui_batch", json::object()},
    };
    for (const auto& b : bad) {
        INFO(b.tool << " " << b.args.dump());
        CHECK_THROWS_AS(spec_of(specs, b.tool).build_argv(b.args, Policy{}), std::invalid_argument);
    }
    CHECK_THROWS_AS(spec_of(specs, "gui_doctor").build_argv(json::array(), Policy{}), std::invalid_argument);
    // null arguments behave like {}
    CHECK_NOTHROW(spec_of(specs, "gui_doctor").build_argv(json(nullptr), Policy{}));
}

TEST_CASE("MCP catalog: error messages are model readable", "[mcp][tools]") {
    const auto specs = read_tool_specs();
    try {
        spec_of(specs, "gui_transaction_start").build_argv(json{{"code", "X"}, {"bogus", 1}}, Policy{});
        FAIL("expected exception");
    } catch (const std::invalid_argument& e) {
        const std::string message = e.what();
        CHECK(message.find("bogus") != std::string::npos);
        CHECK(message.find("allowed") != std::string::npos);
    }
    try {
        spec_of(specs, "gui_transaction_start").build_argv(json::object(), Policy{});
        FAIL("expected exception");
    } catch (const std::invalid_argument& e) {
        CHECK(std::string(e.what()).find("code") != std::string::npos);
    }
}

TEST_CASE("MCP catalog: dry parse against the real CLI11 registry", "[mcp][tools]") {
    const auto specs = read_tool_specs();
    Policy policy_with_default;
    policy_with_default.default_connection = 9;
    Policy json_policy;
    json_policy.default_format = "json";

    std::vector<std::pair<std::string, Argv>> cases;
    for (const auto& sample : samples()) {
        cases.emplace_back(sample.tool, spec_of(specs, sample.tool).build_argv(sample.args, Policy{}));
        cases.emplace_back(sample.tool, spec_of(specs, sample.tool).build_argv(sample.args, policy_with_default));
        cases.emplace_back(sample.tool, spec_of(specs, sample.tool).build_argv(sample.args, json_policy));
    }
    for (const auto& [tool, argv] : cases) {
        INFO(tool << ": " << json(argv).dump());
        CLI::App app{"fairyfly"};
        fairyfly::commands::build_command_tree(app);
        Argv full{"fairyfly"};
        full.insert(full.end(), argv.begin(), argv.end());
        std::vector<const char*> ptrs;
        for (const auto& a : full) ptrs.push_back(a.c_str());
        try {
            app.parse(static_cast<int>(ptrs.size()), ptrs.data());
        } catch (const CLI::ParseError& e) {
            FAIL("CLI rejected the argv: " << e.what());
        }
        // The parsed CLI path must be exactly the command-table path of the tool.
        const auto* command = fairyfly::command_table::find_by_tool(tool);
        REQUIRE(command != nullptr);
        CHECK(fairyfly::commands::invoked_command_path(app) == command->path_string());
        CHECK(fairyfly::command_table::command_of_argv(argv) == command->path_string());
    }
}
