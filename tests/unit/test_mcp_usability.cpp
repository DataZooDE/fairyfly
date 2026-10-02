#include <catch2/catch_test_macros.hpp>

#include <set>
#include <string>
#include <vector>

#include "include/auth/authorize.h"
#include "include/mcp/dispatcher.h"
#include "include/mcp/result_shaper.h"
#include "include/mcp/tool_catalog.h"
#include "include/vkey.h"

using namespace fairyfly::mcp;
using fairyfly::Result;
namespace auth = fairyfly::auth;
using Argv = std::vector<std::string>;

namespace {

Result ok_result(json data = json::object()) {
    Result r;
    r.status = Result::Status::Success;
    r.data = std::move(data);
    return r;
}

std::string first_text(const ToolResult& r) { return r.content.at(0).at("text").get<std::string>(); }

const ToolDef* find_def(const std::vector<ToolDef>& defs, const std::string& name) {
    for (const auto& d : defs)
        if (d.name == name) return &d;
    return nullptr;
}

} // namespace

// ---- item 1: policy-aware texts ------------------------------------------------------------------
TEST_CASE("describe_for drops references to hidden tools", "[mcp][usability]") {
    ToolSpec spec;
    spec.def.description = "ids for gui_element_click / gui_element_get{?gui_element_fill: / gui_element_fill}. Cheap.";
    CHECK(describe_for(spec, {"gui_element_get"}) == "ids for gui_element_click / gui_element_get. Cheap.");
    CHECK(describe_for(spec, {"gui_element_fill"}) ==
          "ids for gui_element_click / gui_element_get / gui_element_fill. Cheap.");
    spec.def.description = "no markers here";
    CHECK(describe_for(spec, {}) == "no markers here");
    spec.def.description = "broken {?tool without end";
    CHECK(describe_for(spec, {}) == "broken {?tool without end");
}

TEST_CASE("tools/list texts never mention a hidden tool", "[mcp][usability]") {
    Policy read_only;  // read-only by default: gui_element_fill is hidden
    CommandDispatcher ro([](const Argv&) { return ok_result(); }, read_only);
    for (const auto& def : ro.list_tools()) {
        INFO(def.name);
        CHECK(def.description.find("gui_element_fill") == std::string::npos);
        CHECK(def.description.find("{?") == std::string::npos);
    }
    Policy writable;
    writable.read_only = false;
    writable.allow_write = true;
    CommandDispatcher rw([](const Argv&) { return ok_result(); }, writable);
    const auto defs = rw.list_tools();
    REQUIRE(find_def(defs, "gui_screen_find"));
    REQUIRE(find_def(defs, "gui_element_fill"));
    INFO(find_def(defs, "gui_screen_find")->description);
    CHECK(find_def(defs, "gui_screen_find")->description.find("gui_element_fill") != std::string::npos);
    CHECK(find_def(defs, "gui_element_f4")->description.find("gui_element_fill") != std::string::npos);

    // a token that is read-only on a writable server also loses the references
    Principal token;
    token.name = "t";
    token.read_only = true;
    const auto for_token = rw.list_tools_for(token);
    REQUIRE(find_def(for_token, "gui_screen_find"));
    CHECK(find_def(for_token, "gui_screen_find")->description.find("gui_element_fill") == std::string::npos);
}

TEST_CASE("adapt_cli_hints rewrites click hints and drops hints of hidden tools", "[mcp][usability][shaper]") {
    const std::string grid =
        "**Usage Examples:**\n```python\n# Read cell value\nfairyfly element get 'wnd[0]/usr/cntl/shellcont/shell' --row 0 --column 'COLUMN_NAME'\n\n"
        "# Set cell value\nfairyfly element fill 'wnd[0]/usr/cntl/shell' 'new_value' --row 0 --column 'COLUMN_NAME'\n\n"
        "# Click cell to select row\nfairyfly element click 'wnd[0]/usr/cntl/shell' --row 0 --column 'COLUMN_NAME'\n```\n\nafter\n";
    const std::set<std::string> read_only = {"gui_element_click", "gui_element_get"};
    const std::string adapted = adapt_cli_hints(grid, read_only);
    CHECK(adapted.find("element fill") == std::string::npos);
    CHECK(adapted.find("Set cell value") == std::string::npos);
    CHECK(adapted.find("fairyfly element") == std::string::npos);
    CHECK(adapted.find("# Click cell to select row\ngui_element_click(element=\"wnd[0]/usr/cntl/shell\", row=0, column=\"COLUMN_NAME\")") !=
          std::string::npos);
    CHECK(adapted.find("after") != std::string::npos);

    const std::set<std::string> writable = {"gui_element_click", "gui_element_get", "gui_element_fill"};
    const std::string with_fill = adapt_cli_hints(grid, writable);
    CHECK(with_fill.find("gui_element_fill(element=\"wnd[0]/usr/cntl/shell\", value=\"new_value\", row=0, column=\"COLUMN_NAME\")") !=
          std::string::npos);

    // inline hints: rewritten when click is visible, whole line dropped when not
    const std::string inline_hint = "x\n_Click with_: `fairyfly element click '@active/<button_path>'`\ny\n";
    CHECK(adapt_cli_hints(inline_hint, read_only) ==
          "x\n_Click with_: `gui_element_click(element=\"@active/<button_path>\")`\ny\n");
    CHECK(adapt_cli_hints(inline_hint, {}) == "x\ny\n");

    // a block whose every line is dropped disappears with its header
    const std::string only_fill = "a\n\n**Usage Examples:**\n```bash\n# Set cell value\nfairyfly element fill 'i' 'v' --row 0 --column 'C'\n```\n\nb\n";
    const std::string gone = adapt_cli_hints(only_fill, read_only);
    CHECK(gone.find("Usage Examples") == std::string::npos);
    CHECK(gone.find("```") == std::string::npos);
    CHECK(gone.find("b") != std::string::npos);

    // text without hints is untouched
    CHECK(adapt_cli_hints("nothing to see", {}) == "nothing to see");
}

TEST_CASE("shape_result adapts hints for Markdown screen results", "[mcp][usability][shaper]") {
    ToolSpec spec;
    spec.def.name = "gui_screen_find";
    spec.output = ToolOutput::Markdown;
    Policy policy;
    json elements = json::array();
    elements.push_back({{"id", "wnd[0]/usr/btnX"}, {"type", "GuiButton"}, {"text", "X"}});
    const Result res = ok_result({{"scanned_count", 1}, {"elements", elements}});
    const std::set<std::string> visible = {"gui_element_click"};
    const auto shaped = shape_result(res, spec, policy, "", &visible);
    CHECK(first_text(shaped).find("element fill") == std::string::npos);
}

// ---- item 2: benign status messages reach the model as success ---------------------------------
TEST_CASE("A click result with a benign warning is shaped as success with the message", "[mcp][usability][status]") {
    Policy policy;
    CommandDispatcher dispatcher(
        [](const Argv&) {
            return ok_result({{"action", "click"},
                              {"status_message", {{"type", "W"}, {"text", "No short dumps match the selection criteria"}}},
                              {"warning", true}});
        },
        policy);
    CallContext ctx;
    ctx.request_id = 1;
    const auto result = dispatcher.call_tool("gui_element_click", {{"element", "wnd[0]/usr/btnTODAY"}}, ctx);
    CHECK_FALSE(result.is_error);
    CHECK(first_text(result).find("No short dumps match") != std::string::npos);
    REQUIRE(result.structured.has_value());
    CHECK((*result.structured)["data"]["status_message"]["type"] == "W");
    CHECK((*result.structured)["data"]["warning"] == true);
}

// ---- item 3: key names ---------------------------------------------------------------------------
TEST_CASE("parse_vkey accepts named page keys and tolerant spellings", "[mcp][usability][vkey]") {
    using fairyfly::sap::parse_vkey;
    struct Row { const char* text; int vkey; };
    const Row rows[] = {
        {"enter", 0}, {"Enter", 0}, {" ENTER ", 0}, {"return", 0},
        {"pageup", 81}, {"page_up", 81}, {"Page-Up", 81}, {"page up", 81}, {"PgUp", 81},
        {"pagedown", 82}, {"page_down", 82}, {"PAGE DOWN", 82}, {"pgdn", 82}, {"pgdown", 82},
        {"pagetop", 80}, {"page_top", 80}, {"ctrl+pageup", 80}, {"Ctrl+PgUp", 80}, {"firstpage", 80},
        {"pagebottom", 83}, {"page_bottom", 83}, {"ctrl+pagedown", 83}, {"lastpage", 83},
        {"f1", 1}, {"F8", 8}, {"f12", 12}, {"shift+f1", 13}, {"Shift+F12", 24}, {"shift_f4", 16}, {"shift-f4", 16},
        {"0", 0}, {"15", 15}, {"82", 82}, {"99", 99},
    };
    for (const auto& row : rows) {
        INFO(row.text);
        REQUIRE(parse_vkey(row.text).has_value());
        CHECK(*parse_vkey(row.text) == row.vkey);
    }
    for (const char* bad : {"", "tab", "up", "down", "left", "arrow_down", "f13", "f22", "f0", "100", "-1", "shift+enter",
                            "pagedownx", "ctrl+s", "banana", "_", "--"}) {
        INFO(bad);
        CHECK_FALSE(parse_vkey(bad).has_value());
    }
}

TEST_CASE("T-code allowlist key policy: spellings, messages, unknown names", "[mcp][usability][vkey][auth]") {
    Principal p;
    p.name = "t";
    p.all_scopes = true;
    p.authenticated = true;
    p.tcodes = {"SM21"};
    Policy policy;
    policy.read_only = false;
    const auto spec = [] {
        for (const auto& s : all_tool_specs())
            if (s.def.name == "gui_key_send") return s;
        return ToolSpec{};
    }();
    auto decide = [&](const std::string& key) {
        return auth::authorize_call(p, spec, spec.family, json{{"key", key}}, policy, std::nullopt, std::string("SM21"));
    };

    for (const char* key : {"pagedown", "page_down", "PageDown", "pgdn", "pageup", "page_up", "pagetop", "pagebottom",
                            "ctrl+pagedown", "enter", "f4", "F8", "82"})
        CHECK(decide(key).allowed);

    // a known but navigating key is a policy denial that lists the exact accepted spellings
    const auto f3 = decide("f3");
    CHECK_FALSE(f3.allowed);
    CHECK(f3.code == "TCODE_DENIED");
    for (const char* spelling : {"enter", "f4", "f8", "pagedown", "page_down", "pgdn", "pageup", "pagetop", "pagebottom"})
        CHECK(f3.message.find(spelling) != std::string::npos);

    // every spelling printed in the denial message really is accepted
    const std::string spellings = auth::tcode_safe_key_spellings();
    for (const char* name : {"enter", "f4", "f8", "pageup", "page_up", "pgup", "pagedown", "page_down", "pgdn", "pagetop",
                             "ctrl+pageup", "pagebottom", "ctrl+pagedown"}) {
        INFO(name);
        CHECK(spellings.find(name) != std::string::npos);
        CHECK(auth::tcode_safe_key(name));
    }

    // an unknown name is INVALID_ARGUMENT with the supported names, not TCODE_DENIED
    for (const char* key : {"f22", "tab", "arrow_down", "bogus"}) {
        INFO(key);
        const auto d = decide(key);
        CHECK_FALSE(d.allowed);
        CHECK(d.code == "INVALID_ARGUMENT");
        CHECK(d.message.find("pagedown") != std::string::npos);
        CHECK(d.message.find(key) != std::string::npos);
    }
}

TEST_CASE("gui_key_send builds the key argv and rejects unknown names", "[mcp][usability][vkey]") {
    for (const auto& spec : all_tool_specs()) {
        if (spec.def.name != "gui_key_send") continue;
        Policy policy;
        CHECK(spec.build_argv(json{{"key", "page_down"}}, policy) == Argv{"key", "send", "page_down"});
        try {
            spec.build_argv(json{{"key", "f22"}}, policy);
            FAIL("expected invalid_argument");
        } catch (const std::invalid_argument& e) {
            CHECK(std::string(e.what()).find("pagedown") != std::string::npos);
        }
        CHECK(spec.def.description.find("pagedown") != std::string::npos);
        CHECK(spec.def.description.find("arrow") != std::string::npos);
    }
}
