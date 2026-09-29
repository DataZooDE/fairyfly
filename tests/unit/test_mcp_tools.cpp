#include <catch2/catch_test_macros.hpp>

#include <CLI/CLI.hpp>
#include <regex>
#include <set>

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
        {"sap_doctor", json::object(), {"doctor"}},
        {"sap_sessions", json::object(), {"list"}},
        {"sap_connections", json::object(), {"connections"}},
        {"sap_connections", {{"cleanup", true}}, {"connections", "--cleanup"}},
        {"sap_attach", {{"session_id", "/app/con[0]/ses[0]"}}, {"attach", "--session-id", "/app/con[0]/ses[0]"}},
        {"sap_launch", {{"name", "PRD"}}, {"launch", "PRD"}},
        {"sap_launch",
         {{"name", "PRD"}, {"login", true}, {"credential", "c1"}, {"multiple_logon", "keep"}, {"allow_sapshcut", true}},
         {"launch", "PRD", "--login", "--credential", "c1", "--multiple-logon", "keep", "--allow-sapshcut"}},
        {"sap_login", json::object(), {"login"}},
        {"sap_login", {{"connection", 2}, {"credential", "c"}, {"multiple_logon", "terminate"}},
         {"login", "--connection", "2", "--credential", "c", "--multiple-logon", "terminate"}},
        {"sap_tcode", {{"code", "SE38"}}, {"tcode", "SE38"}},
        {"sap_tcode", {{"code", "/nSM37"}, {"connection", 3}}, {"tcode", "/nSM37", "--connection", "3"}},
        {"sap_screen_read", json::object(), {"screen", "read", "--max-rows", "20", "--compact", "--output", "markdown"}},
        {"sap_screen_read",
         {{"tab", "tabpTAB2"}, {"only", "buttons"}, {"text_contains", "Save"}, {"id_contains", "btn"},
          {"type", "GuiButton"}, {"first", true}, {"max_rows", 5}, {"skip_trees", true}, {"probe_all", true},
          {"format", "json"}, {"compact", false}, {"connection", 1}},
         {"screen", "read", "--tab", "tabpTAB2", "--only-buttons", "--text-contains", "Save", "--id-contains", "btn",
          "--type", "GuiButton", "--first", "--max-rows", "5", "--skip-trees", "--probe-all", "--connection", "1",
          "--output", "json"}},
        {"sap_screen_read", {{"no_tabs", true}, {"only", "editable"}},
         {"screen", "read", "--no-tabs", "--only-editable", "--max-rows", "20", "--compact", "--output", "markdown"}},
        {"sap_screen_read", {{"only", "fields"}, {"text_contains", "-danger"}},
         {"screen", "read", "--only-fields", "--text-contains=-danger", "--max-rows", "20", "--compact", "--output", "markdown"}},
        {"sap_screen_read", {{"only", "f4_fields"}},
         {"screen", "read", "--only-f4-fields", "--max-rows", "20", "--compact", "--output", "markdown"}},
        {"sap_screen_find", {{"name_contains", "Save"}},
         {"screen", "find", "--name-contains", "Save", "--limit", "10", "--output", "markdown"}},
        {"sap_screen_find", {{"id_contains", "btn"}, {"type", "GuiButton"}, {"limit", 3}, {"probe_all", true}, {"connection", 0}},
         {"screen", "find", "--id-contains", "btn", "--type", "GuiButton", "--limit", "3", "--probe-all", "--connection", "0",
          "--output", "markdown"}},
        {"sap_get", {{"element", "wnd[0]/usr/txtX"}}, {"get", "wnd[0]/usr/txtX"}},
        {"sap_get", {{"element", "/app/con[0]/ses[0]/wnd[0]/usr/cntl"}, {"list_nodes", true}},
         {"get", "/app/con[0]/ses[0]/wnd[0]/usr/cntl", "--list-nodes"}},
        {"sap_menu_list", json::object(), {"screen", "menu"}},
        {"sap_menu_list", {{"window", "@active"}}, {"screen", "menu", "--window", "@active"}},
        {"sap_capture", json::object(), {"screen", "capture", "--format", "base64"}},
        {"sap_capture", {{"scale", 0.5}, {"x", 1}, {"y", 2}, {"width", 3}, {"height", 4}},
         {"screen", "capture", "--format", "base64", "--scale", "0.5", "--x", "1", "--y", "2", "--width", "3", "--height", "4"}},
        {"sap_capture", {{"scale", 800}}, {"screen", "capture", "--format", "base64", "--scale", "800"}},
        {"sap_credentials_list", json::object(), {"credentials", "list"}},
        {"sap_click", {{"element", "wnd[0]/tbar[1]/btn[8]"}}, {"click", "wnd[0]/tbar[1]/btn[8]"}},
        {"sap_click",
         {{"element", "wnd[0]/usr/tree"}, {"connection", 1}, {"wait_for_window", true}, {"timeout_ms", 3000}, {"row", 2},
          {"column", "COL"}, {"doubleclick", true}, {"node_key", "K"}, {"tree_action", "select"}, {"menu_item", "M"}},
         {"click", "wnd[0]/usr/tree", "--connection", "1", "--wait-for-window", "--timeout", "3000", "--row", "2",
          "--column", "COL", "--doubleclick", "--node-key", "K", "--tree-action", "select", "--menu-item", "M"}},
        {"sap_send_key", {{"key", "enter"}}, {"send-key", "enter"}},
        {"sap_send_key", {{"key", "f8"}, {"window", "wnd[1]"}, {"connection", 2}},
         {"send-key", "f8", "--window", "wnd[1]", "--connection", "2"}},
        {"sap_close_popup", json::object(), {"close"}},
        {"sap_close_popup", {{"vkey", 0}}, {"close", "--vkey", "0"}},
        {"sap_press_f4", {{"element", "wnd[0]/usr/ctxtF"}}, {"press_f4", "wnd[0]/usr/ctxtF"}},
        {"sap_menu_select", {{"path", "Table/Save"}}, {"screen", "menu", "--select", "Table/Save"}},
        {"sap_menu_select", {{"path", "System/Log off"}, {"window", "@active"}},
         {"screen", "menu", "--select", "System/Log off", "--window", "@active"}},
        {"sap_disconnect", json::object(), {"disconnect"}},
        {"sap_disconnect", {{"connection", 4}, {"close_session", true}}, {"disconnect", "--connection", "4", "--close-session"}},
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
        CHECK(spec.def.name.rfind("sap_", 0) == 0);
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
    for (const char* name : {"sap_doctor", "sap_sessions", "sap_connections", "sap_attach", "sap_launch", "sap_login",
                             "sap_tcode", "sap_screen_read", "sap_screen_find", "sap_get", "sap_menu_list", "sap_capture",
                             "sap_credentials_list", "sap_click", "sap_send_key", "sap_close_popup", "sap_press_f4",
                             "sap_menu_select", "sap_disconnect", "sap_batch"})
        CHECK(seen.count(name) == 1);
}

TEST_CASE("MCP catalog: batch annotations and destructive flags", "[mcp][tools]") {
    const auto specs = read_tool_specs();
    const auto& batch = spec_of(specs, "sap_batch");
    CHECK(batch.def.annotations["destructiveHint"] == true);
    CHECK(batch.def.annotations["readOnlyHint"] == false);
    CHECK(spec_of(specs, "sap_click").def.annotations["destructiveHint"] == true);
    CHECK(spec_of(specs, "sap_send_key").def.annotations["destructiveHint"] == true);
    CHECK(spec_of(specs, "sap_menu_select").def.annotations["destructiveHint"] == true);
    CHECK(spec_of(specs, "sap_screen_read").def.annotations["readOnlyHint"] == true);
    // Documented as destructive.
    CHECK(spec_of(specs, "sap_launch").def.input_schema["properties"]["multiple_logon"]["enum"].size() == 4);
    CHECK(spec_of(specs, "sap_launch").def.description.find("DESTRUCTIVE") != std::string::npos);
    CHECK(spec_of(specs, "sap_disconnect").def.description.find("DESTRUCTIVE") != std::string::npos);
    CHECK(spec_of(specs, "sap_screen_read").def.description.find("max_rows") != std::string::npos);
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
    const auto& capture = spec_of(specs, "sap_capture");
    for (const auto& args : {json::object(), json{{"scale", 0.3}}, json{{"x", 1}, {"y", 2}, {"width", 3}, {"height", 4}}}) {
        for (const auto& token : capture.build_argv(args, Policy{})) {
            CHECK(token != "--file");
            CHECK(token != "-f");
            CHECK(token != "--show");
        }
    }
    CHECK_THROWS_AS(capture.build_argv(json{{"file", "x.png"}}, Policy{}), std::invalid_argument);
    CHECK_THROWS_AS(capture.build_argv(json{{"show", true}}, Policy{}), std::invalid_argument);
    CHECK_THROWS_AS(spec_of(specs, "sap_login").build_argv(json{{"password", "x"}}, Policy{}), std::invalid_argument);
    CHECK_THROWS_AS(spec_of(specs, "sap_login").build_argv(json{{"credentials_file", "x"}}, Policy{}), std::invalid_argument);
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
    CHECK(spec_of(specs, "sap_tcode").build_argv(json{{"code", "SE38"}}, policy) ==
          Argv{"tcode", "SE38", "--connection", "7"});
    CHECK(spec_of(specs, "sap_tcode").build_argv(json{{"code", "SE38"}, {"connection", 2}}, policy) ==
          Argv{"tcode", "SE38", "--connection", "2"});
    // Tools without session context never get one.
    CHECK(spec_of(specs, "sap_sessions").build_argv(json::object(), policy) == Argv{"list"});
    CHECK(spec_of(specs, "sap_attach").build_argv(json{{"session_id", "s"}}, policy) == Argv{"attach", "--session-id", "s"});
    Policy json_policy;
    json_policy.default_format = "json";
    CHECK(spec_of(specs, "sap_screen_read").build_argv(json::object(), json_policy).back() == "json");
    CHECK(spec_of(specs, "sap_screen_read").build_argv(json{{"format", "markdown"}}, json_policy).back() == "markdown");
}

TEST_CASE("MCP catalog: invalid arguments throw std::invalid_argument", "[mcp][tools]") {
    const auto specs = read_tool_specs();
    struct Bad { std::string tool; json args; };
    const std::vector<Bad> bad = {
        {"sap_doctor", {{"x", 1}}},
        {"sap_tcode", json::object()},
        {"sap_tcode", {{"code", ""}}},
        {"sap_tcode", {{"code", "-x"}}},
        {"sap_tcode", {{"code", 5}}},
        {"sap_tcode", {{"code", "SE38"}, {"connection", "1"}}},
        {"sap_tcode", {{"code", "SE38"}, {"connection", -1}}},
        {"sap_tcode", {{"code", "SE38"}, {"connection", 1.5}}},
        {"sap_screen_read", {{"max_rows", 0}}},
        {"sap_screen_read", {{"max_rows", 201}}},
        {"sap_screen_read", {{"only", "everything"}}},
        {"sap_screen_read", {{"format", "toon"}}},
        {"sap_screen_read", {{"tab", "t"}, {"no_tabs", true}}},
        {"sap_screen_read", {{"first", "yes"}}},
        {"sap_screen_find", json::object()},
        {"sap_screen_find", {{"name_contains", "a"}, {"limit", 101}}},
        {"sap_screen_find", {{"name_contains", "a"}, {"limit", 0}}},
        {"sap_get", json::object()},
        {"sap_get", {{"element", "--list-nodes"}}},
        {"sap_click", {{"element", "wnd[0]/usr/btn"}, {"tree_action", "explode"}}},
        {"sap_click", {{"element", "wnd[0]/usr/btn"}, {"timeout_ms", 1}}},
        {"sap_click", {{"element", "wnd[0]/usr/btn"}, {"row", -1}}},
        {"sap_send_key", json::object()},
        {"sap_close_popup", {{"vkey", 100}}},
        {"sap_launch", {{"name", "PRD"}, {"multiple_logon", "destroy"}}},
        {"sap_launch", json::object()},
        {"sap_launch", {{"name", "-PRD"}}},
        {"sap_attach", json::object()},
        {"sap_attach", {{"session_id", ""}}},
        {"sap_menu_select", json::object()},
        {"sap_capture", {{"scale", 0}}},
        {"sap_capture", {{"width", 0}}},
        {"sap_disconnect", {{"close_session", "true"}}},
        {"sap_batch", json::object()},
    };
    for (const auto& b : bad) {
        INFO(b.tool << " " << b.args.dump());
        CHECK_THROWS_AS(spec_of(specs, b.tool).build_argv(b.args, Policy{}), std::invalid_argument);
    }
    CHECK_THROWS_AS(spec_of(specs, "sap_doctor").build_argv(json::array(), Policy{}), std::invalid_argument);
    // null arguments behave like {}
    CHECK_NOTHROW(spec_of(specs, "sap_doctor").build_argv(json(nullptr), Policy{}));
}

TEST_CASE("MCP catalog: error messages are model readable", "[mcp][tools]") {
    const auto specs = read_tool_specs();
    try {
        spec_of(specs, "sap_tcode").build_argv(json{{"code", "X"}, {"bogus", 1}}, Policy{});
        FAIL("expected exception");
    } catch (const std::invalid_argument& e) {
        const std::string message = e.what();
        CHECK(message.find("bogus") != std::string::npos);
        CHECK(message.find("allowed") != std::string::npos);
    }
    try {
        spec_of(specs, "sap_tcode").build_argv(json::object(), Policy{});
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
        app.allow_windows_style_options(false);
        fairyfly::commands::register_all_commands();
        fairyfly::commands::CommandRegistry::instance().setup_all_commands(app);
        Argv full{"fairyfly"};
        full.insert(full.end(), argv.begin(), argv.end());
        std::vector<const char*> ptrs;
        for (const auto& a : full) ptrs.push_back(a.c_str());
        try {
            app.parse(static_cast<int>(ptrs.size()), ptrs.data());
        } catch (const CLI::ParseError& e) {
            FAIL("CLI rejected the argv: " << e.what());
        }
        std::string invoked;
        for (const auto& command : fairyfly::commands::CommandRegistry::instance().all_commands())
            if (command->was_invoked()) invoked = command->name();
        CHECK(invoked == argv[0]);
    }
}
