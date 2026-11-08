// Local tokens with a T-code allowlist (src/auth/authorize.cpp): the two click rules the gateway's libffl check_call
// enforces since fairyfly-gateway #251 (2026-11-08), so the HTTP server refuses the same clicks as the link.
#include <catch2/catch_test_macros.hpp>

#include "include/auth/authorize.h"
#include "include/mcp/policy.h"
#include "include/mcp/tool_catalog.h"

#include "policy.hpp"  // libffl (vendored): the gateway's own check, for the parity test

#include "support/tool_specs.h"

#include <optional>
#include <string>

using namespace fairyfly;
using namespace fairyfly::mcp;
using fairyfly::auth::authorize_call;
using fairyfly::test::spec_of;

namespace {

Principal allowlisted(std::vector<std::string> tcodes) {
    Principal p;
    p.name = "clicks";
    p.all_scopes = false;
    p.scopes = {"element", "batch"};
    p.authenticated = true;
    p.read_only = false;
    p.tcodes = std::move(tcodes);
    return p;
}

PolicyDecision click(const Principal& p, const json& args) {
    const ToolSpec& spec = spec_of("gui_element_click");
    return authorize_call(p, spec, spec.family, args, Policy{}, std::nullopt, std::string("ME21N"));
}

PolicyDecision batch(const Principal& p, const json& items) {
    const ToolSpec& spec = spec_of("gui_batch");
    return authorize_call(p, spec, spec.family, {{"items", items}}, Policy{}, std::nullopt, std::string("ME21N"));
}

const char* const kSaveIds[] = {
    "wnd[0]/tbar[0]/btn[11]",
    "/app/con[0]/ses[0]/wnd[0]/tbar[0]/btn[11]",
    "@active/wnd[0]/tbar[0]/btn[11]",
    "WND[0]/TBAR[0]/BTN[11]",
    "wnd[1]/tbar[0]/btn[11]",
    "wnd[0]/tbar[0]/btn[011]",
    "wnd[0]/tbar[ 0 ]/btn[ 11 ]",
    "  wnd[0]/tbar[00]/btn[0011]  ",
};

const char* const kMenuBarIds[] = {
    "wnd[0]/mbar/menu[0]/menu[3]",
    "wnd[0]/mbar/menu[4]",
    "/app/con[0]/ses[0]/wnd[0]/mbar/menu[1]",
    "WND[0]/MBAR/MENU[0]",
    "wnd[0]/mbar",
};

const char* const kOtherButtons[] = {
    "wnd[0]/tbar[0]/btn[3]",      // Back
    "wnd[0]/tbar[0]/btn[12]",     // Cancel
    "wnd[0]/tbar[0]/btn[110]",    // not btn[11]
    "wnd[0]/tbar[1]/btn[8]",      // application toolbar
    "wnd[0]/usr/btnSAVE_LIKE",    // a screen button with a lookalike name
    "wnd[0]/usr/btnMBAR",         // "mbar" inside a name, not a path segment
    "wnd[0]/usr/subMBARX/btnA",
};

}  // namespace

TEST_CASE("authorize: a T-code allowlist refuses clicks on the system toolbar's Save button", "[auth][authz][navigation][tcode_click]") {
    const Principal p = allowlisted({"ME21N"});
    for (const char* id : kSaveIds)
        for (const char* key : {"element", "id", "element_id"}) {
            INFO(key << " = " << id);
            const auto d = click(p, {{key, id}});
            CHECK_FALSE(d.allowed);
            CHECK(d.code == "TCODE_DENIED");
            CHECK(d.message.find("sav") != std::string::npos);
        }
    // as a batch step too
    const auto d = batch(p, json::array({{{"tool", "gui_element_click"}, {"arguments", {{"element", "wnd[0]/usr/btnOK"}}}},
                                         {{"tool", "gui_element_click"}, {"arguments", {{"element", "wnd[0]/tbar[0]/btn[11]"}}}}}));
    CHECK_FALSE(d.allowed);
    CHECK(d.code == "TCODE_DENIED");
    CHECK(d.message.find("batch item 2") != std::string::npos);
}

TEST_CASE("authorize: a T-code allowlist refuses clicks on menu bar items", "[auth][authz][navigation][tcode_click]") {
    const Principal p = allowlisted({"ME21N"});
    for (const char* id : kMenuBarIds)
        for (const char* key : {"element", "id", "element_id"}) {
            INFO(key << " = " << id);
            const auto d = click(p, {{key, id}});
            CHECK_FALSE(d.allowed);
            CHECK(d.code == "TCODE_DENIED");
            CHECK(d.message.find("gui_transaction_start") != std::string::npos);
        }
    const auto d = batch(p, json::array({{{"tool", "gui_element_click"}, {"arguments", {{"id", "wnd[0]/mbar/menu[0]/menu[2]"}}}}}));
    CHECK_FALSE(d.allowed);
    CHECK(d.code == "TCODE_DENIED");
}

TEST_CASE("authorize: the click rules leave other buttons and tokens without a list alone", "[auth][authz][navigation][tcode_click]") {
    const Principal listed = allowlisted({"ME21N"});
    for (const char* id : kOtherButtons) {
        INFO(id);
        CHECK(click(listed, {{"element", id}}).allowed);
    }
    CHECK(batch(listed, json::array({{{"tool", "gui_element_click"}, {"arguments", {{"element", "wnd[0]/tbar[0]/btn[3]"}}}}})).allowed);

    // no transaction list: the Save button and the menu bar stay clickable
    const Principal free = allowlisted({});
    for (const char* id : kSaveIds) {
        INFO(id);
        CHECK(click(free, {{"element", id}}).allowed);
    }
    for (const char* id : kMenuBarIds) {
        INFO(id);
        CHECK(click(free, {{"element", id}}).allowed);
    }

    // --allow-navigation restores gui_menu_select and every key (Ctrl+S included), so it restores both clicks too
    Principal nav = allowlisted({"ME21N"});
    nav.allow_navigation = true;
    CHECK(click(nav, {{"element", "wnd[0]/tbar[0]/btn[11]"}}).allowed);
    CHECK(click(nav, {{"element", "wnd[0]/mbar/menu[0]"}}).allowed);
}

TEST_CASE("authorize: the local click rules agree with libffl's check_call", "[auth][authz][navigation][tcode_click][libffl]") {
    ffy::policy::Grants g;
    g.tools = {"*"};
    g.tcodes = {"ME21N"};
    g.allow_write = true;
    const Principal p = allowlisted({"ME21N"});
    auto all = std::vector<std::string>(std::begin(kSaveIds), std::end(kSaveIds));
    all.insert(all.end(), std::begin(kMenuBarIds), std::end(kMenuBarIds));
    all.insert(all.end(), std::begin(kOtherButtons), std::end(kOtherButtons));
    for (const auto& id : all) {
        INFO(id);
        const auto gateway = ffy::policy::check_call(g, "gui_element_click", {{"element", id}});
        const auto local = click(p, {{"element", id}});
        CHECK(local.allowed == gateway.ok());
        if (!gateway.ok()) CHECK(local.code == gateway.code);
    }
}
