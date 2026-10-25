#include <catch2/catch_test_macros.hpp>

#include <stdexcept>

#include "include/audit_log.h"
#include "include/auth/authorize.h"
#include "include/fill_fields.h"
#include "include/mcp/policy.h"
#include "include/mcp/tool_catalog.h"
#include "support/tool_specs.h"

// Multi-field fill: `element fill --fields` / gui_element_fill `fields` (+ then_key). Pure parsing, argv, audit and
// authorization; no SAP.

using namespace fairyfly;
using namespace fairyfly::mcp;
using Argv = std::vector<std::string>;

namespace {

const ToolSpec& fill_spec() { return fairyfly::test::spec_of("gui_element_fill"); }

Argv fill_argv(const json& args) { return fill_spec().build_argv(args, Policy{}); }

Principal tcode_token(bool read_only, bool selection_input) {
    Principal p;
    p.name = "t";
    p.id = "t-id";
    p.all_scopes = false;
    p.scopes = {"element", "screen", "key", "transaction"};
    p.read_only = read_only;
    p.authenticated = true;
    p.tcodes = {"SE16"};
    p.allow_selection_input = selection_input;
    return p;
}

PolicyDecision decide(const Principal& p, const json& args) {
    Policy policy;
    policy.read_only = false;
    return auth::authorize_call(p, fill_spec(), fill_spec().family, args, policy, std::string("A4H/001"), std::string("SE16"));
}

}  // namespace

TEST_CASE("fill fields: parsing and validation", "[fill][fields]") {
    std::string why;
    auto ok = sap::parse_fill_fields(json::parse(R"([{"element":"wnd[0]/usr/ctxtA","value":"1"},
        {"id":"wnd[0]/usr/ctxtB","clear":true},
        {"element":"wnd[0]/usr/cntl/shell","row":2,"column":"AMOUNT","value":"X","checkbox":true,"commit":true}])"), why);
    REQUIRE(ok);
    REQUIRE(ok->size() == 3);
    CHECK((*ok)[0].value == "1");
    CHECK((*ok)[1].element == "wnd[0]/usr/ctxtB");
    CHECK((*ok)[1].clear);
    CHECK((*ok)[2].row == 2);
    CHECK((*ok)[2].column == "AMOUNT");
    CHECK((*ok)[2].checkbox);

    auto bad = [&](const char* text) {
        why.clear();
        CHECK_FALSE(sap::parse_fill_fields(json::parse(text), why));
        CHECK_FALSE(why.empty());
        return why;
    };
    CHECK(bad("[]").find("1 to 50") != std::string::npos);
    CHECK(bad("{}").find("array") != std::string::npos);
    CHECK(bad(R"([{"value":"1"}])").find("'element' is required") != std::string::npos);
    CHECK(bad(R"([{"element":"a"}])").find("'value'") != std::string::npos);
    CHECK(bad(R"([{"element":"a","value":"1","clear":true}])").find("not both") != std::string::npos);
    CHECK(bad(R"([{"element":"a","value":1}])").find("string") != std::string::npos);
    CHECK(bad(R"([{"element":"a","value":"1","row":1}])").find("row") != std::string::npos);
    CHECK(bad(R"([{"element":"a","value":"maybe","row":1,"column":"C","checkbox":true}])").find("checkbox") != std::string::npos);
    CHECK(bad(R"([{"element":"a","value":"1","color":"red"}])").find("unknown property 'color'") != std::string::npos);
    CHECK(bad(R"([{"element":"a","id":"b","value":"1"}])").find("aliases") != std::string::npos);
    json many = json::array();
    for (int i = 0; i < 51; ++i) many.push_back({{"element", "a"}, {"value", "1"}});
    CHECK_FALSE(sap::parse_fill_fields(many, why));

    CHECK(sap::fill_then_vkey("enter") == 0);
    CHECK(sap::fill_then_vkey("F8") == 8);
    CHECK_FALSE(sap::fill_then_vkey("ctrl+s"));
    CHECK_FALSE(sap::fill_then_vkey("f3"));
    CHECK_FALSE(sap::fill_then_vkey("11"));
}

TEST_CASE("fill fields: MCP arguments map to one element fill --fields call", "[fill][fields][mcp]") {
    const json fields = json::parse(R"([{"element":"wnd[0]/usr/ctxtA","value":"1"},{"element":"wnd[0]/usr/ctxtB","clear":true}])");
    CHECK(fill_argv({{"fields", fields}}) == Argv{"element", "fill", "--fields", fields.dump()});
    CHECK(fill_argv({{"fields", fields}, {"then_key", "f8"}, {"stop_on_error", false}, {"connection", 2}}) ==
          Argv{"element", "fill", "--fields", fields.dump(), "--then-key", "f8", "--keep-going", "--connection", "2"});
    CHECK(fill_argv({{"fields", fields}, {"stop_on_error", true}}) == Argv{"element", "fill", "--fields", fields.dump()});

    CHECK_THROWS_AS(fill_argv({{"fields", fields}, {"then_key", "save"}}), std::invalid_argument);
    CHECK_THROWS_AS(fill_argv({{"fields", fields}, {"element", "wnd[0]/usr/ctxtA"}}), std::invalid_argument);
    CHECK_THROWS_AS(fill_argv({{"fields", json::array()}}), std::invalid_argument);
    CHECK_THROWS_AS(fill_argv({{"element", "wnd[0]/usr/ctxtA"}, {"value", "1"}, {"then_key", "enter"}}), std::invalid_argument);
    // the single-field form is unchanged
    CHECK(fill_argv({{"element", "wnd[0]/usr/ctxtA"}, {"value", "1"}}) == Argv{"element", "fill", "wnd[0]/usr/ctxtA", "1"});

    const auto& schema = fill_spec().def.input_schema;
    REQUIRE(schema["properties"].contains("fields"));
    CHECK(schema["properties"]["fields"]["maxItems"] == 50);
    CHECK(schema["properties"]["then_key"]["enum"] == json::array({"enter", "f8"}));
    CHECK(fill_spec().def.description.find("WHOLE SCREEN IN ONE CALL") != std::string::npos);
}

TEST_CASE("fill fields: the audit trail never records the values", "[fill][fields][audit]") {
    const std::string fields = R"([{"element":"wnd[0]/usr/ctxtA","value":"hunter2"}])";
    CHECK(audit::redact_argv({"element", "fill", "--fields", fields, "--then-key", "f8", "--connection", "1"}) ==
          Argv{"element", "fill", "--fields", "<redacted>", "--then-key", "f8", "--connection", "1"});
    CHECK(audit::redact_argv({"element", "fill", "--fields=" + fields}) == Argv{"element", "fill", "--fields=<redacted>"});
}

TEST_CASE("fill fields: every entry meets the token rules", "[fill][fields][auth]") {
    // T-code token in write mode: the command field is blocked in any entry
    const Principal writer = tcode_token(false, false);
    CHECK(decide(writer, {{"fields", {{{"element", "wnd[0]/usr/ctxtA"}, {"value", "1"}}}}}).allowed);
    const auto okcd = decide(writer, {{"fields", {{{"element", "wnd[0]/usr/ctxtA"}, {"value", "1"}},
                                                  {{"element", "wnd[0]/tbar[0]/okcd"}, {"value", "/nSU01"}}}}});
    CHECK_FALSE(okcd.allowed);
    CHECK(okcd.code == "TCODE_DENIED");

    // selection input: grid cells, the command field and credential fields are refused in any entry
    const Principal typist = tcode_token(true, true);
    auto denied = [&](const json& entry) {
        const auto d = decide(typist, {{"fields", {{{"element", "wnd[0]/usr/ctxtA"}, {"value", "1"}}, entry}}});
        INFO(entry.dump());
        CHECK_FALSE(d.allowed);
        CHECK(d.code == "INPUT_TARGET_DENIED");
    };
    denied({{"element", "wnd[0]/usr/cntl/shell"}, {"row", 0}, {"column", "C"}, {"value", "1"}});
    denied({{"element", "wnd[0]/tbar[0]/okcd"}, {"value", "/nSU01"}});
    denied({{"element", "wnd[0]/usr/pwdRSYST-BCODE"}, {"value", "x"}});
    CHECK(decide(typist, {{"fields", "nope"}}).code == "INPUT_TARGET_DENIED");

    // a read-only token without the option never fills, also not with fields
    const auto ro = decide(tcode_token(true, false), {{"fields", {{{"element", "wnd[0]/usr/ctxtA"}, {"value", "1"}}}}});
    CHECK_FALSE(ro.allowed);
}

TEST_CASE("fill: one field is validated by one rule set for the CLI, MCP and the fields list", "[fill]") {
    // review 2026-10-25: tool_catalog_write, fill_command and parse_fill_fields each checked value/clear/grid by hand
    std::string why;
    const auto ok = sap::parse_fill_field(json{{"element", "wnd[0]/usr/txtA"}, {"value", "v"}}, why);
    REQUIRE(ok);
    CHECK(ok->element == "wnd[0]/usr/txtA");
    CHECK(ok->value == "v");
    const auto cell = sap::parse_fill_field(json{{"element", "g"}, {"value", "X"}, {"row", 2}, {"column", "C"}, {"checkbox", true}}, why);
    REQUIRE(cell);
    CHECK(cell->row == 2);
    CHECK(cell->checkbox);
    CHECK_FALSE(sap::parse_fill_field(json{{"element", "e"}, {"value", "v"}, {"checkbox", true}}, why));
    CHECK(why == "GridView cells need both 'row' and 'column' (also required with 'checkbox' or 'commit')");
    CHECK_FALSE(sap::parse_fill_field(json{{"element", "e"}}, why));
    CHECK(why == "provide 'value' or set 'clear' to true");
    CHECK_FALSE(sap::parse_fill_field(json{{"element", "e"}, {"value", "maybe"}, {"row", 1}, {"column", "C"}, {"checkbox", true}}, why));
    CHECK(why == "with 'checkbox', 'value' must be X, 1, true (checked) or 0, false (unchecked)");
    // the list keeps naming the field
    std::string list_why;
    CHECK_FALSE(sap::parse_fill_fields(json::array({json{{"element", "e"}}}), list_why));
    CHECK(list_why == "fields[0]: provide 'value' or set 'clear' to true");
}
